// This is molecular (particle) dynamics (MD)

#include "particle.h"
#include "vertex.h"
#include "interface.h"
#include "thermostat.h"
#include "control.h"
#include "forces.h"
#include "energies.h"
#include "functions.h"

void
md(vector<PARTICLE> &ion, INTERFACE &box, vector<THERMOSTAT> &real_bath, vector<DATABIN> &bin, CONTROL &mdremote,
   string &simulationParams, double charge_meshpoint, int valency_counterion, bool screen) {

    mpi::environment env;
    mpi::communicator world;
    //Boundary calculation
    unsigned int range = ion.size() / world.size() + 1.5;
    unsigned int lowerBound = world.rank() * range;
    unsigned int upperBound = (world.rank() + 1) * range - 1;
    unsigned int extraElements = world.size() * range - ion.size();
    unsigned int sizFVec = upperBound - lowerBound + 1;
    if (world.rank() == world.size() - 1) {
        upperBound = ion.size() - 1;
        sizFVec = upperBound - lowerBound + 1 + extraElements;
    }
    if (world.size() == 1) {
        lowerBound = 0;
        upperBound = ion.size() - 1;
    }
    std::vector<VECTOR3D> partialForceVector(ion.size() + extraElements, VECTOR3D(0, 0, 0));
    std::vector<VECTOR3D> lj_ion_ion(sizFVec, VECTOR3D(0, 0, 0));
    std::vector<VECTOR3D> lj_ion_leftdummy(sizFVec, VECTOR3D(0, 0, 0));
    std::vector<VECTOR3D> lj_ion_left_wall(sizFVec, VECTOR3D(0, 0, 0));
    std::vector<VECTOR3D> lj_ion_rightdummy(sizFVec, VECTOR3D(0, 0, 0));
    std::vector<VECTOR3D> lj_ion_right_wall(sizFVec, VECTOR3D(0, 0, 0));
    std::vector<VECTOR3D> sendForceVector(sizFVec, VECTOR3D(0, 0, 0));
    std::vector <VECTOR3D> coulomb_rightwallForce(sizFVec, VECTOR3D(0, 0, 0));
    std::vector <VECTOR3D> coulomb_leftwallForce(sizFVec, VECTOR3D(0, 0, 0));

    initialize_particle_velocities(ion, real_bath);    // particle velocities initialized
    for_md_calculate_force(ion, box, 'y', lowerBound, upperBound, partialForceVector, lj_ion_ion, lj_ion_leftdummy,
                           lj_ion_left_wall, lj_ion_rightdummy,
                           lj_ion_right_wall, sendForceVector, coulomb_rightwallForce, coulomb_leftwallForce, charge_meshpoint, valency_counterion);        // force on particles initialized
    long double particle_ke = particle_kinetic_energy(ion);// compute initial kinetic energy

    long double potential_energy;

    vector<double> ion_energy(sizFVec, 0.0);
    vector<double> lj_ion_ion_energy(sizFVec, 0.0);
    vector<double> lj_ion_leftdummy_energy(sizFVec, 0.0);
    vector<double> lj_ion_leftwall_energy(sizFVec, 0.0);
    vector<double> lj_ion_rightdummy_energy(sizFVec, 0.0);
    vector<double> lj_ion_rightwall_energy(sizFVec, 0.0);
    vector <double> coulomb_rightwall_energy(sizFVec, 0.0);
    vector <double> coulomb_leftwall_energy(sizFVec, 0.0);

    // compute initial potential energy
    potential_energy = energy_functional(ion, box, lowerBound, upperBound, ion_energy, lj_ion_ion_energy,
                                         lj_ion_leftdummy_energy, lj_ion_leftwall_energy, lj_ion_rightdummy_energy,
                                         lj_ion_rightwall_energy, coulomb_rightwall_energy, coulomb_leftwall_energy, charge_meshpoint, valency_counterion);

    // Output md essentials
    if (world.rank() == 0) {
        cout << "\n";
        cout << "Propagation of ions using Molecular Dynamics method" << " begins " << endl;
        cout << "Time step in the simulation " << mdremote.timestep << endl;
        cout << "Total number of simulation steps " << mdremote.steps << endl;

        // Output md essentials
        if (mdremote.verbose) {
            cout << "Initial ion kinetic energy " << particle_ke << endl;
            cout << "Inital potential energy " << potential_energy << endl;
            cout << "Initial system energy " << particle_ke + potential_energy << endl;
            cout << "Chain length (L+1) implementation " << real_bath.size() << endl;
            cout << "Main thermostat temperature " << real_bath[0].T << endl;
            cout << "Main thermostat mass " << real_bath[0].Q << endl;
            cout << "Number of bins used for computing density profiles " << bin.size() << endl;
            cout << "Production begins at " << mdremote.hiteqm << endl;
            cout << "Sampling frequency " << mdremote.freq << endl;
            cout << "Extra computation every " << mdremote.extra_compute << " steps" << endl;
            cout << "Write density profile every " << mdremote.writedensity << endl;
        }
    }
    // for movie
    int moviestart = 1;                    // starting point of the movie

    // for energy
    double energy_samples = 0;

    // for density profile
    vector<double> mean_positiveion_density;            // average density profile
    vector<double> mean_negativeion_density;            // average density profile
    vector<double> mean_sq_positiveion_density;            // average of square of density
    vector<double> mean_sq_negativeion_density;            // average of square of density
    for (unsigned int b = 0; b < bin.size(); b++) {
        mean_positiveion_density.push_back(0.0);
        mean_negativeion_density.push_back(0.0);
        mean_sq_positiveion_density.push_back(0.0);
        mean_sq_negativeion_density.push_back(0.0);
    }

    double density_profile_samples = 0;            // number of samples used to estimate density profile

    long double expfac_real;                // exponential factors useful in velocity Verlet routine

    double percentage = 0, percentagePre = -1;


    // Part II : Propagate
    unsigned int i = 0;
    for (int num = 1; num <= mdremote.steps; num++) {
        // INTEGRATOR
        //! begins
        // reverse update of Nose-Hoover chain
        for (int j = real_bath.size() - 1; j > -1; j--)
            update_chain_xi(j, real_bath, mdremote.timestep, particle_ke);
        for (unsigned int j = 0; j < real_bath.size(); j++)
            real_bath[j].update_eta(mdremote.timestep);

        expfac_real = exp(-0.5 * mdremote.timestep * real_bath[0].xi);

        // core loop: velocity --> position --> force --> velocity
        //#pragma omp parallel for schedule(dynamic) private(i)
        for (i = 0; i < ion.size(); i++)
            ion[i].new_update_velocity(mdremote.timestep, real_bath[0], expfac_real);
        //#pragma omp parallel for schedule(dynamic) private(i)
        for (i = 0; i < ion.size(); i++)
            ion[i].update_position(mdremote.timestep);
        for_md_calculate_force(ion, box, 'y', lowerBound, upperBound, partialForceVector, lj_ion_ion, lj_ion_leftdummy,
                               lj_ion_left_wall, lj_ion_rightdummy,
                               lj_ion_right_wall, sendForceVector, coulomb_rightwallForce, coulomb_leftwallForce, charge_meshpoint, valency_counterion);        // force on particles initialized
        //#pragma omp parallel for schedule(dynamic) private(i)
        for (i = 0; i < ion.size(); i++)
            ion[i].new_update_velocity(mdremote.timestep, real_bath[0], expfac_real);

        // kinetic energies needed to set canonical ensemble
        particle_ke = particle_kinetic_energy(ion);

        // forward update of Nose-Hoover chain
        for (unsigned int j = 0; j < real_bath.size(); j++)
            real_bath[j].update_eta(mdremote.timestep);
        for (unsigned int j = 0; j < real_bath.size(); j++)
            update_chain_xi(j, real_bath, mdremote.timestep, particle_ke);
        //! ends

        // extra computations
        if (num == 1 || num % mdremote.extra_compute == 0) {
            energy_samples++;
            compute_n_write_useful_data(num, ion, real_bath, box, lowerBound, upperBound, ion_energy, lj_ion_ion_energy,
                                        lj_ion_leftdummy_energy, lj_ion_leftwall_energy, lj_ion_rightdummy_energy,
                                        lj_ion_rightwall_energy, coulomb_rightwall_energy, coulomb_leftwall_energy, charge_meshpoint, valency_counterion);
        }

        // make a movie
        if (num >= moviestart && num % mdremote.moviefreq == 0)
            make_movie(num, ion, box);

        // compute density profile
        if (num >= mdremote.hiteqm && (num % mdremote.freq == 0)) {
            density_profile_samples++;
            compute_density_profile(num, density_profile_samples, mean_positiveion_density, mean_sq_positiveion_density,
                                    mean_negativeion_density, mean_sq_negativeion_density, ion, box, bin, mdremote, screen);
        }

        if (world.rank() == 0) {
            //percentage calculation
            if (!mdremote.verbose)
                percentage = roundf(num / (double) mdremote.steps * 100);
            else
                percentage = roundf(num / (double) mdremote.steps * 100 * 10) / 10;
            //percentage output
            if (percentage != percentagePre) {
                if (!mdremote.verbose) {
                    int progressBarVal = (int) (percentage + 0.5);
                    printf("=RAPPTURE-PROGRESS=>%d Simulation Running...\n", progressBarVal);
                } else {
                    double fraction_completed = percentage / 100;
                    ProgressBar(fraction_completed);
                }
                percentagePre = percentage;

            }
        }
    }
    average_errorbars_density(density_profile_samples, mean_positiveion_density,mean_sq_positiveion_density,mean_negativeion_density,
                                    mean_sq_negativeion_density, ion, box, bin, simulationParams, screen);


      if (world.rank() == 0) {

        string finalConFilePath = rootDirectory + "outfiles/final_configuration.dat";
        ofstream final_configuration(finalConFilePath.c_str());
        for (unsigned int i = 0; i < ion.size(); i++)
            final_configuration << ion[i].posvec << endl;

        if (mdremote.verbose) {
            cout << "Number of samples used to compute energy" << setw(10) << energy_samples << endl;
            cout << "Number of samples used to get density profile" << setw(10) << density_profile_samples << endl;
        }
        cout << "Dynamics of ions simulated for " << mdremote.steps * mdremote.timestep * unittime * 1e9
             << " nanoseconds" << endl;

    }
    return;
}
// End of MD routine
