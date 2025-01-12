#include "continuum_integration.hpp"
namespace SPH
{
//=================================================================================================//
namespace continuum_dynamics
{
ContinuumInitialCondition::ContinuumInitialCondition(SPHBody &sph_body)
    : LocalDynamics(sph_body), PlasticContinuumDataSimple(sph_body),
      pos_(particles_->pos_), vel_(particles_->vel_), stress_tensor_3D_(particles_->stress_tensor_3D_) {}
//=================================================================================================//
ShearAccelerationRelaxation::ShearAccelerationRelaxation(BaseInnerRelation &inner_relation)
    : fluid_dynamics::BaseIntegration<ContinuumDataInner>(inner_relation),
      continuum_(DynamicCast<GeneralContinuum>(this, particles_->getBaseMaterial())),
      G_(continuum_.getShearModulus(continuum_.getYoungsModulus(), continuum_.getPoissonRatio())),
      smoothing_length_(sph_body_.sph_adaptation_->ReferenceSmoothingLength()), shear_stress_(particles_->shear_stress_),
      acc_shear_(particles_->acc_shear_) {}
//=================================================================================================//
void ShearAccelerationRelaxation::interaction(size_t index_i, Real dt)
{
    Real rho_i = rho_[index_i];
    Vecd acceleration = Vecd::Zero();
    Neighborhood &inner_neighborhood = inner_configuration_[index_i];
    for (size_t n = 0; n != inner_neighborhood.current_size_; ++n)
    {
        size_t index_j = inner_neighborhood.j_[n];
        Real r_ij = inner_neighborhood.r_ij_[n];
        Real dW_ijV_j = inner_neighborhood.dW_ijV_j_[n];
        Vecd &e_ij = inner_neighborhood.e_ij_[n];
        Real eta_ij = 2 * (0.7 * (Real)Dimensions + 2.1) * (vel_[index_i] - vel_[index_j]).dot(e_ij) / (r_ij + TinyReal);
        acceleration += eta_ij * dW_ijV_j * e_ij;
    }
    acc_shear_[index_i] += G_ * acceleration * dt / rho_i;
}
//=================================================================================================//
ShearStressRelaxation ::
    ShearStressRelaxation(BaseInnerRelation &inner_relation)
    : fluid_dynamics::BaseIntegration<ContinuumDataInner>(inner_relation),
      continuum_(DynamicCast<GeneralContinuum>(this, particles_->getBaseMaterial())),
      shear_stress_(particles_->shear_stress_), shear_stress_rate_(particles_->shear_stress_rate_),
      velocity_gradient_(particles_->velocity_gradient_), strain_tensor_(particles_->strain_tensor_),
      strain_tensor_rate_(particles_->strain_tensor_rate_), von_mises_stress_(particles_->von_mises_stress_),
      von_mises_strain_(particles_->von_mises_strain_), Vol_(particles_->Vol_),
      B_(*particles_->getVariableByName<Matd>("KernelCorrectionMatrix")) {}
//====================================================================================//
void ShearStressRelaxation::initialization(size_t index_i, Real dt)
{
    strain_tensor_[index_i] += strain_tensor_rate_[index_i] * 0.5 * dt;
    shear_stress_[index_i] += shear_stress_rate_[index_i] * 0.5 * dt;
}
//====================================================================================//
void ShearStressRelaxation::interaction(size_t index_i, Real dt)
{
    Matd velocity_gradient = Matd::Zero();
    Neighborhood &inner_neighborhood = inner_configuration_[index_i];
    for (size_t n = 0; n != inner_neighborhood.current_size_; ++n)
    {
        size_t index_j = inner_neighborhood.j_[n];
        Real dW_ijV_j_ = inner_neighborhood.dW_ijV_j_[n];
        Vecd &e_ij = inner_neighborhood.e_ij_[n];
        Vecd v_ij = vel_[index_i] - vel_[index_j];
        velocity_gradient -= v_ij * (B_[index_i] * e_ij * dW_ijV_j_).transpose();
    }
    velocity_gradient_[index_i] = velocity_gradient;
    /*calculate strain*/
    Matd strain_rate = 0.5 * (velocity_gradient + velocity_gradient.transpose());
    strain_tensor_rate_[index_i] = strain_rate;
    strain_tensor_[index_i] += strain_tensor_rate_[index_i] * 0.5 * dt;
    Matd strain_i = strain_tensor_[index_i];
    von_mises_strain_[index_i] = getVonMisesStressFromMatrix(strain_i);
}
//====================================================================================//
void ShearStressRelaxation::update(size_t index_i, Real dt)
{
    shear_stress_rate_[index_i] = continuum_.ConstitutiveRelationShearStress(velocity_gradient_[index_i], shear_stress_[index_i]);
    shear_stress_[index_i] += shear_stress_rate_[index_i] * dt * 0.5;
    Matd stress_tensor_i = shear_stress_[index_i] - p_[index_i] * Matd::Identity();
    von_mises_stress_[index_i] = getVonMisesStressFromMatrix(stress_tensor_i);
}
//====================================================================================//
StressDiffusion::StressDiffusion(BaseInnerRelation &inner_relation)
    : BasePlasticIntegration<PlasticContinuumDataInner>(inner_relation),
      fai_(DynamicCast<PlasticContinuum>(this, plastic_continuum_).getFrictionAngle()),
      smoothing_length_(sph_body_.sph_adaptation_->ReferenceSmoothingLength()),
      sound_speed_(plastic_continuum_.ReferenceSoundSpeed()) {}
//====================================================================================//
void StressDiffusion::interaction(size_t index_i, Real dt)
{
    Vecd acc_prior_i = force_prior_[index_i] / mass_[index_i];
    Real gravity = abs(acc_prior_i(1, 0));
    Real density = plastic_continuum_.getDensity();
    Mat3d diffusion_stress_rate_ = Mat3d::Zero();
    Mat3d diffusion_stress_ = Mat3d::Zero();
    Neighborhood &inner_neighborhood = inner_configuration_[index_i];
    for (size_t n = 0; n != inner_neighborhood.current_size_; ++n)
    {
        size_t index_j = inner_neighborhood.j_[n];
        Real r_ij = inner_neighborhood.r_ij_[n];
        Real dW_ijV_j = inner_neighborhood.dW_ijV_j_[n];
        Real y_ij = pos_[index_i](1, 0) - pos_[index_j](1, 0);
        diffusion_stress_ = stress_tensor_3D_[index_i] - stress_tensor_3D_[index_j];
        diffusion_stress_(0, 0) -= (1 - sin(fai_)) * density * gravity * y_ij;
        diffusion_stress_(1, 1) -= density * gravity * y_ij;
        diffusion_stress_(2, 2) -= (1 - sin(fai_)) * density * gravity * y_ij;
        diffusion_stress_rate_ += 2 * zeta_ * smoothing_length_ * sound_speed_ *
                                  diffusion_stress_ * r_ij * dW_ijV_j / (r_ij * r_ij + 0.01 * smoothing_length_);
    }
    stress_rate_3D_[index_i] = diffusion_stress_rate_;
}
//====================================================================================//
} // namespace continuum_dynamics
} // namespace SPH
