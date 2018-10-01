#include <AMReX_Array.H>
#include <AMReX_BC_TYPES.H>
#include <AMReX_BLassert.H>
#include <AMReX_Box.H>
#include <AMReX_EBMultiFabUtil.H>
#include <AMReX_MultiFab.H>
#include <AMReX_ParmParse.H>
#include <AMReX_VisMF.H>

#include <incflo_level.H>
#include <advance_F.H>
#include <mac_F.H>
#include <projection_F.H>
#include <setup_F.H>

void incflo_level::Advance(
	int lev, int nstep, int steady_state, Real& dt, Real& prev_dt, Real time, Real stop_time)
{
	AMREX_ALWAYS_ASSERT(lev == 0);

	BL_PROFILE_REGION_START("incflo::Advance");
	BL_PROFILE("incflo::Advance");

	amrex::Print() << "\n ============   NEW TIME STEP   ============ \n";

	// Extrapolate boundary values for density and volume fraction
	fill_mf_bc(lev, *mu[lev]);

	// Fill ghost nodes and reimpose boundary conditions
	incflo_set_scalar_bcs(lev);
	incflo_set_velocity_bcs(lev, 0);

	// Start loop: if we are not seeking a steady state solution,
	// the loop will execute only once
	int keep_looping = 1;
	int iter = 1;
	do
	{
        // Compute time step size
		incflo_compute_dt(lev, time, stop_time, steady_state, dt);

		if(steady_state)
		{
			amrex::Print() << "\n   Iteration " << iter << " with dt = " << dt << "\n" << std::endl;
		}
		else
		{
			amrex::Print() << "\n   Step " << nstep + 1 << ": from old_time " << time
						   << " to new time " << time + dt << " with dt = " << dt << "\n"
						   << std::endl;
		}

		// Backup field variable to old
		MultiFab::Copy(*p_o[lev], *p[lev], 0, 0, p[lev]->nComp(), p_o[lev]->nGrow());
		MultiFab::Copy(*ro_o[lev], *ro[lev], 0, 0, ro[lev]->nComp(), ro_o[lev]->nGrow());
		MultiFab::Copy(*vel_o[lev], *vel[lev], 0, 0, vel[lev]->nComp(), vel_o[lev]->nGrow());

		// Time integration step
		//
		// Create temporary multifabs to hold the old-time conv and divtau
		//    so we don't have to re-compute them in the corrector
		MultiFab conv_old(grids[lev], dmap[lev], 3, 0, MFInfo(), *ebfactory[lev]);
		MultiFab divtau_old(grids[lev], dmap[lev], 3, 0, MFInfo(), *ebfactory[lev]);

		// Predictor step
		bool proj_2 = true;
		incflo_apply_predictor(lev, conv_old, divtau_old, dt, proj_2);

		// Print info about predictor step
        if(verbose > 0)
		{
			amrex::Print() << "\nAfter predictor step:\n";
			incflo_print_max_vel(lev);
			incflo_compute_divu(lev);
			amrex::Print() << "max(abs(divu)) = " << incflo_norm0(divu, lev, 0) << "\n";
		}

		// Corrector step
		proj_2 = true;
		incflo_apply_corrector(lev, conv_old, divtau_old, dt, proj_2);

		// Print info about corrector step
        if(verbose > 0)
		{
			amrex::Print() << "\nAfter corrector step:\n";
			incflo_print_max_vel(lev);
			incflo_compute_divu(lev);
			amrex::Print() << "max(abs(divu)) = " << incflo_norm0(divu, lev, 0) << "\n";
		}

		//
		// Check whether to exit the loop or not
		//
		if(steady_state)
			keep_looping = !steady_state_reached(lev, dt, iter);
		else
			keep_looping = 0;

		// Update interations count
		++iter;
	} while(keep_looping);

    prev_dt = dt;

	BL_PROFILE_REGION_STOP("incflo::Advance");
}

void incflo_level::incflo_compute_dt(int lev, Real time, Real stop_time, int steady_state, Real& dt)
{
	// DT is always computed even for fixed dt, so we can
	// issue a warning if fixed dt does not satisfy CFL condition.
	Real dt_new = dt;

	// Compute dt for this time step
	Real umax = incflo_norm0(vel, lev, 0);
	Real vmax = incflo_norm0(vel, lev, 1);
	Real wmax = incflo_norm0(vel, lev, 2);
	Real romax = incflo_norm0(ro, lev, 0);
	Real mumax = incflo_norm0(mu, lev, 0);

	Real gradp0max[3];

    gradp0max[0] = incflo_norm0(gp0, lev, 0);
    gradp0max[1] = incflo_norm0(gp0, lev, 1);
    gradp0max[2] = incflo_norm0(gp0, lev, 2);

	ParallelDescriptor::ReduceRealMax(gradp0max[0]);
	ParallelDescriptor::ReduceRealMax(gradp0max[1]);
	ParallelDescriptor::ReduceRealMax(gradp0max[2]);

	compute_new_dt(&umax,
				   &vmax,
				   &wmax,
				   &romax,
				   &mumax,
				   gradp0max,
				   geom[lev].CellSize(),
				   &cfl,
				   &steady_state,
				   &time,
				   &stop_time,
				   &dt_new);

	if(fixed_dt > 0.)
	{
		if(dt_new < fixed_dt)
		{
			amrex::Print() << "WARNING: fixed_dt does not satisfy CFL condition: "
						   << "max dt by CFL     : " << dt_new << "\n"
						   << "fixed dt specified: " << fixed_dt << std::endl;
		}
		dt = fixed_dt;
	}
	else
	{
		dt = dt_new;
	}
}

//
// Compute predictor:
//
//  1. Compute
//
//     vel = vel_o + dt * R_u^n + dt * divtau*(1/ro)
//
//  2. Add explicit forcing term ( AKA gravity, lagged pressure gradient)
//
//     vel = vel + dt * ( g - grad(p+p0)/ro)
//
//  3. Add implicit forcing term 
//  
//     vel = vel / ( 1 + dt * f_gds/ro )
//
//  4. Solve for phi
//
//     div( grad(phi) / ro ) = div( vel / dt + grad(p)/ro )
//
//  5. Compute
//
//     vel = vel -  dt * grad(phi) / ro
//
//  6. Define
//
//     p = phi
//
void incflo_level::incflo_apply_predictor(
	int lev, MultiFab& conv_old, MultiFab& divtau_old, amrex::Real dt, bool proj_2)
{
	// Compute the explicit advective term R_u^n
	incflo_compute_ugradu_predictor(lev, conv_old, vel_o);

	// If explicit_diffusion == true  then we compute the full diffusive terms
	// here
	// If explicit_diffusion == false then we compute only the off-diagonal terms
	// here
	incflo_compute_divtau(lev, divtau_old, vel_o);

	// First add the convective term
	MultiFab::Saxpy(*vel[lev], dt, conv_old, 0, 0, 3, 0);

	// Add the diffusion terms (either all if explicit_diffusion == true or just
	// the
	//    off-diagonal terms if explicit_diffusion == false)
	MultiFab::Saxpy(*vel[lev], dt, divtau_old, 0, 0, 3, 0);

	// Add the forcing terms
	incflo_apply_forcing_terms(lev, dt, vel);

	// Convert velocities to momenta
	for(int n = 0; n < 3; n++)
		MultiFab::Multiply(*vel[lev], (*ro[lev]), 0, n, 1, vel[lev]->nGrow());

	// Add (-dt grad p to momenta)
	MultiFab::Saxpy(*vel[lev], -dt, *gp[lev], 0, 0, 3, vel[lev]->nGrow());
	MultiFab::Saxpy(*vel[lev], -dt, *gp0[lev], 0, 0, 3, vel[lev]->nGrow());

	// Convert momenta back to velocities
	for(int n = 0; n < 3; n++)
		MultiFab::Divide(*vel[lev], (*ro[lev]), 0, n, 1, vel[lev]->nGrow());

	// If doing implicit diffusion, solve here for u^*
	if(!explicit_diffusion)
		incflo_diffuse_velocity(lev, dt);

	// Project velocity field
	incflo_apply_projection(lev, dt, proj_2);
}

//
// Compute corrector:
//
//  1. Compute
//
//     vel = vel_o + dt * (R_u^* + R_u^n) / 2 + dt * divtau*(1/ro)
//
//     where the starred variables are computed using "predictor-step"
//     variables.
//
//  2. Add explicit forcing term ( AKA gravity, lagged pressure gradient )
//
//     vel = vel + dt * ( g - grad(p+p0)/ro)
//
//  3. Add implicit forcing term 
//
//     vel = vel / ( 1 + dt * f_gds/ro )
//
//  4. Solve for phi
//
//     div( grad(phi) / ro ) = div(  vel / dt + grad(p)/ro )
//
//  5. Compute
//
//     vel = vel -  dt * grad(phi) / ro
//
//  6. Define
//
//     p = phi
//
void incflo_level::incflo_apply_corrector(
	int lev, MultiFab& conv_old, MultiFab& divtau_old, amrex::Real dt, bool proj_2)
{
	BL_PROFILE("incflo_level::incflo_apply_corrector");

	MultiFab conv(grids[lev], dmap[lev], 3, 0);
	MultiFab divtau(grids[lev], dmap[lev], 3, 0, MFInfo(), *ebfactory[lev]);

	// Compute the explicit advective term R_u^*
	incflo_compute_ugradu_corrector(lev, conv, vel);

	// If explicit_diffusion == true  then we compute the full diffusive terms
	// here
	// If explicit_diffusion == false then we compute only the off-diagonal terms
	// here
	incflo_compute_divtau(lev, divtau, vel);

	// Define u = u_o + dt/2 (R_u^* + R_u^n)
	MultiFab::LinComb(*vel[lev], 1.0, *vel_o[lev], 0, dt / 2.0, conv, 0, 0, 3, 0);
	MultiFab::Saxpy(*vel[lev], dt / 2.0, conv_old, 0, 0, 3, 0);

	// Add the diffusion terms (either all if explicit_diffusion == true or just
	// the
	//    off-diagonal terms if explicit_diffusion == false)
	MultiFab::Saxpy(*vel[lev], dt / 2.0, divtau, 0, 0, 3, 0);
	MultiFab::Saxpy(*vel[lev], dt / 2.0, divtau_old, 0, 0, 3, 0);

	// Add forcing terms
	incflo_apply_forcing_terms(lev, dt, vel);

	// Convert velocities to momenta
	for(int n = 0; n < 3; n++)
		MultiFab::Multiply(*vel[lev], (*ro[lev]), 0, n, 1, vel[lev]->nGrow());

	// Add (-dt grad p to momenta)
	MultiFab::Saxpy(*vel[lev], -dt, *gp[lev], 0, 0, 3, vel[lev]->nGrow());
	MultiFab::Saxpy(*vel[lev], -dt, *gp0[lev], 0, 0, 3, vel[lev]->nGrow());

	// Convert momenta back to velocities
	for(int n = 0; n < 3; n++)
		MultiFab::Divide(*vel[lev], (*ro[lev]), 0, n, 1, vel[lev]->nGrow());

	// If doing implicit diffusion, solve here for u^*
	if(!explicit_diffusion)
		incflo_diffuse_velocity(lev, dt);

	// Apply projection
	incflo_apply_projection(lev, dt, proj_2);
}

void incflo_level::incflo_apply_forcing_terms(int lev,
											  amrex::Real dt,
											  Vector<std::unique_ptr<MultiFab>>& vel)
{
	BL_PROFILE("incflo_level::incflo_apply_forcing_terms");

	Box domain(geom[lev].Domain());

#ifdef _OPENMP
#pragma omp parallel
#endif
	for(MFIter mfi(*vel[lev], true); mfi.isValid(); ++mfi)
	{
		// Tilebox
		Box bx = mfi.tilebox();

		add_forcing(BL_TO_FORTRAN_BOX(bx),
					BL_TO_FORTRAN_ANYD((*vel[lev])[mfi]),
					BL_TO_FORTRAN_ANYD((*ro[lev])[mfi]),
					domain.loVect(),
					domain.hiVect(),
					geom[lev].CellSize(),
					&dt);
	}
}

//
// Check if steady state has been reached by verifying that
//
//      max(abs( u^(n+1) - u^(n) )) < tol * dt
//      max(abs( v^(n+1) - v^(n) )) < tol * dt
//      max(abs( w^(n+1) - w^(n) )) < tol * dt
//
int incflo_level::steady_state_reached(int lev, Real dt, int iter)
{
	// Make sure velocity is up to date
	incflo_set_velocity_bcs(lev, 0);

    // Use temporaries to store the difference between current and previous solution
	MultiFab temp_vel(vel[lev]->boxArray(), vel[lev]->DistributionMap(), 3, 0);
	MultiFab::LinComb(temp_vel, 1.0, *vel[lev], 0, -1.0, *vel_o[lev], 0, 0, 3, 0);

	MultiFab tmp;
    tmp.define(grids[lev], dmap[lev], 1, 0);
	MultiFab::LinComb(tmp, 1.0, *p[lev], 0, -1.0, *p_o[lev], 0, 0, 1, 0);

	Real delta_u = incflo_norm0(temp_vel, lev, 0);
	Real delta_v = incflo_norm0(temp_vel, lev, 1);
	Real delta_w = incflo_norm0(temp_vel, lev, 2);
	Real delta_p = incflo_norm0(tmp, lev, 0);

	Real tol = steady_state_tol;

	int condition1 = (delta_u < tol * dt) && (delta_v < tol * dt) && (delta_w < tol * dt);

	// Second stop condition
	Real du_n1 = incflo_norm1(temp_vel, lev, 0);
	Real dv_n1 = incflo_norm1(temp_vel, lev, 1);
	Real dw_n1 = incflo_norm1(temp_vel, lev, 2);
	Real dp_n1 = incflo_norm1(tmp, lev, 0);
	Real uo_n1 = incflo_norm1(vel_o, lev, 0);
	Real vo_n1 = incflo_norm1(vel_o, lev, 1);
	Real wo_n1 = incflo_norm1(vel_o, lev, 2);
	Real po_n1 = incflo_norm1(p_o, lev, 0);

	Real tmp1, tmp2, tmp3, tmp4;

	Real local_tol = 1.0e-8;

	if(uo_n1 < local_tol)
		tmp1 = 0.0;
	else
		tmp1 = du_n1 / uo_n1;

	if(vo_n1 < local_tol)
		tmp2 = 0.0;
	else
		tmp2 = dv_n1 / vo_n1;

	if(wo_n1 < local_tol)
		tmp3 = 0.0;
	else
		tmp3 = dw_n1 / wo_n1;

	if(po_n1 < local_tol)
		tmp4 = 0.0;
	else
		tmp4 = dp_n1 / po_n1;

	int condition2 = (tmp1 < tol) && (tmp2 < tol) && (tmp3 < tol); // && (tmp4 < tol);

	// Print out info on steady state checks
    if(verbose)
    {
        amrex::Print() << "\nSteady state check:\n";
        amrex::Print() << "||u-uo||/||uo|| , du/dt  = " << tmp1 << " , " << delta_u / dt << "\n";
        amrex::Print() << "||v-vo||/||vo|| , dv/dt  = " << tmp2 << " , " << delta_v / dt << "\n";
        amrex::Print() << "||w-wo||/||wo|| , dw/dt  = " << tmp3 << " , " << delta_w / dt << "\n";
        amrex::Print() << "||p-po||/||po|| , dp/dt  = " << tmp4 << " , " << delta_p / dt << "\n";
    }

	// Always return negative to first access. This way
	// initial zero velocity field do not test for false positive
	if(iter == 1)
		return 0;
	else
		return condition1 || condition2;
}

void incflo_level::check_for_nans(int lev)
{
	bool ug_has_nans = vel[lev]->contains_nan(0);
	bool vg_has_nans = vel[lev]->contains_nan(1);
	bool wg_has_nans = vel[lev]->contains_nan(2);
	bool pg_has_nans = p[lev]->contains_nan(0);

	if(ug_has_nans)
		amrex::Print() << "WARNING: u contains NaNs!!!";

	if(vg_has_nans)
		amrex::Print() << "WARNING: v contains NaNs!!!";

	if(wg_has_nans)
		amrex::Print() << "WARNING: w contains NaNs!!!";

	if(pg_has_nans)
		amrex::Print() << "WARNING: p contains NaNs!!!";
}

//
// Print the maximum values of the velocity components
//
void incflo_level::incflo_print_max_vel(int lev)
{
	amrex::Print() << "max(abs(u/v/w/p))  = " 
                   << incflo_norm0(vel, lev, 0) << "  "
				   << incflo_norm0(vel, lev, 1) << "  " 
                   << incflo_norm0(vel, lev, 2) << "  "
				   << incflo_norm0(p, lev, 0) << "  " << std::endl;
}