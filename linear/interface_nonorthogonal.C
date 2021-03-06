#include <cassert>
#include <stdexcept>

#include "helpers.h"
#include "iterator.h"
#include "../utils.h"


namespace libresponse {

void solve_linear_response(
    arma::cube &results,
    MatVec_i *matvec,
    SolverIterator_nonorthogonal *solver_iterator,
    const arma::cube &C,
    const arma::umat &fragment_occupations,
    const arma::uvec &occupations,
    const arma::cube &F,
    const arma::mat &S,
    const std::vector<double> &omega,
    std::vector<operator_spec> &operators,
    const configurable &cfg
    )
{

    assert(occupations.n_elem == 4);

    if (omega.empty())
        throw std::runtime_error("Supply one or more frequencies.");
    if (operators.empty())
        throw std::runtime_error("Supply one or more operators.");

    // For cubes, alpha/beta is each slice.
    // For matrices, alpha/beta is each column.
    const size_t nden = C.n_slices;
    const size_t nbasis = C.n_rows;
    const size_t norb = C.n_cols;

    assert(nden == 1 || nden == 2);

    size_t tot_n_slices = 0;
    for (size_t i = 0; i < operators.size(); i++)
        tot_n_slices += operators[i].integrals_ao.n_slices;

    // Store the final scalar values in cubes, where the rows are the
    // property vectors, the columns are the gradient/response
    // vectors, and each slice corresponds to a separate frequency.
    arma::cube results_alph(tot_n_slices, tot_n_slices, omega.size());
    arma::cube results_beta;
    if (nden == 2)
        results_beta.set_size(tot_n_slices, tot_n_slices, omega.size());

    const size_t nocc_alph = occupations(0);
    const size_t nvirt_alph = occupations(1);
    assert(norb == (nocc_alph + nvirt_alph));
    const size_t nov_alph = nocc_alph * nvirt_alph;
    const size_t nocc_beta = occupations(2);
    const size_t nvirt_beta = occupations(3);
    assert(norb == (nocc_beta + nvirt_beta));
    const size_t nov_beta = nocc_beta * nvirt_beta;
    if (nocc_alph == nocc_beta)
        assert(nov_alph == nov_beta);

    // Now that our inputs are guaranteed to be consistent, set up
    // some quanities for printing.
    const int print_level = cfg.get_param<int>("print_level");
    const std::vector<std::string> operator_labels = make_operator_label_vec(operators);
    const std::vector<std::string> component_labels = make_operator_component_vec(operators);

    // Maximum number of iterations and DIIS convergence
    // criterion.
    const std::string solver = cfg.get_param("solver");
    const unsigned maxiter = cfg.get_param<unsigned>("maxiter");
    const int conv_int = cfg.get_param<int>("conv");
    const double conv = std::pow(10.0, -conv_int);

    const std::string hamiltonian = cfg.get_param("hamiltonian");
    const std::string spin = cfg.get_param("spin");

    if (print_level >= 1) {
        std::ostringstream ss;
        ss << " " << dashes << std::endl;
        ss << "  Settings" << std::endl;
        ss << "   nocc_alph: " << nocc_alph << std::endl;
        ss << "   nvirt_alph: " << nvirt_alph << std::endl;
        ss << "   nocc_beta: " << nocc_beta << std::endl;
        ss << "   nvirt_beta: " << nvirt_beta << std::endl;
        ss << "   nov_alph: " << nov_alph << std::endl;
        ss << "   nov_beta: " << nov_beta << std::endl;
        ss << "   Orbital Hessian: " << to_upper(hamiltonian) << std::endl;
        ss << "   Operator spin type: " << spin << std::endl;
        ss << "   Max. iter: " << maxiter << std::endl;
        ss << "   Convergence threshold: 10^" << -conv_int << std::endl;
        ss << "   Frequencies: ";
        for (size_t i = 0; i < omega.size(); i++)
            ss << omega[i] << " ";
        ss << std::endl;
        std::cout << ss.str();
    }

    // TODO does this call copy constructors?
    arma::mat C_occ_alph(C(arma::span::all, arma::span(0, nocc_alph - 1), arma::span(0)));
    arma::mat C_virt_alph(C(arma::span::all, arma::span(nocc_alph, norb - 1), arma::span(0)));
    arma::mat C_occ_beta;
    arma::mat C_virt_beta;
    if (nden == 2) {
        C_occ_beta = C(arma::span::all, arma::span(0, nocc_beta - 1), arma::span(1));
        C_virt_beta = C(arma::span::all, arma::span(nocc_beta, norb - 1), arma::span(1));
    }

    // Form the MO-basis overlap matrices.
    arma::mat sigma_alph = C.slice(0).t() * S * C.slice(0);
    arma::mat sigma_beta;
    if (nden == 2)
        sigma_beta = C.slice(1).t() * S * C.slice(1);

    // Form the full MO-basis Fock matrices.
    arma::mat F_alph = C.slice(0).t() * F.slice(0) * C.slice(0);
    arma::mat F_beta;
    if (nden == 2)
        F_beta = C.slice(1).t() * F.slice(1) * C.slice(1);

    if (print_level >= 10) {
        pretty_print(sigma_alph, "sigma_alph");
        if (nden == 2)
            pretty_print(sigma_beta, "sigma_beta");
        pretty_print(F_alph, "F_alph");
        if (nden == 2)
            pretty_print(F_beta, "F_beta");
    }

    const bool do_canonical_orthogonalization = cfg.get_param<bool>("_do_orthogonalization_canonical");
    arma::mat S_inv;
    arma::mat sigma_inv_alph;
    arma::mat sigma_inv_beta;
    if (do_canonical_orthogonalization) {
        S_inv = arma::pinv(S);
        sigma_inv_alph = arma::pinv(sigma_alph);
        if (nden == 2)
            sigma_inv_beta = arma::pinv(sigma_beta);
        if (print_level >= 10) {
            pretty_print(S_inv, "S_inv");
            pretty_print(sigma_inv_alph, "sigma_inv_alph");
            if (nden == 2)
                pretty_print(sigma_inv_beta, "sigma_inv_beta");
        }
    }

    const arma::uvec norb_frgm = fragment_occupations.col(1);
    const arma::uvec nocc_frgm_alph = fragment_occupations.col(2);
    const arma::uvec nocc_frgm_beta = fragment_occupations.col(3);
    const arma::uvec nvirt_frgm_alph = norb_frgm - nocc_frgm_alph;
    const arma::uvec nvirt_frgm_beta = norb_frgm - nocc_frgm_beta;

    const int frgm_response_idx = cfg.get_param<int>("_frgm_response_idx");
    arma::uvec indices_mo_alph;
    arma::uvec indices_mo_beta;
    if (frgm_response_idx > 0) {
        const type::indices indices_mo_allfrgm_alph = make_indices_mo_restricted_local_occ_all_virt(nocc_frgm_alph, nvirt_frgm_beta);
        const type::indices indices_mo_allfrgm_beta = make_indices_mo_restricted_local_occ_all_virt(nocc_frgm_beta, nvirt_frgm_beta);
        indices_mo_alph = indices_mo_allfrgm_alph.at(frgm_response_idx - 1);
        indices_mo_beta = indices_mo_allfrgm_beta.at(frgm_response_idx - 1);
    } else {
        indices_mo_alph = make_indices_mo_restricted(nocc_frgm_alph, nvirt_frgm_alph);
        indices_mo_beta = make_indices_mo_restricted(nocc_frgm_beta, nvirt_frgm_beta);
    }
    type::indices indices_mo;
    indices_mo.push_back(indices_mo_alph);
    indices_mo.push_back(indices_mo_beta);

    if (print_level >= 10) {
        indices_mo_alph.print("indices_mo_alph");
        indices_mo_beta.print("indices_mo_beta");
    }

    // Form the full 1-electron terms on the LHS that would be MO
    // energy differences in orthogonal response as just a diagonal
    // matrix -> vector.  These don't change during iterations or for
    // different operators, so form them outside any loops.
    arma::mat ediff_alph(nov_alph, nov_alph);
    arma::mat ediff_beta;
    form_ediff_terms(ediff_alph, F_alph, sigma_alph, nocc_alph, nvirt_alph);
    if (nden == 2) {
        ediff_beta.set_size(nov_beta, nov_beta);
        form_ediff_terms(ediff_beta, F_beta, sigma_beta, nocc_beta, nvirt_beta);
    }

    if (print_level >= 10) {
        pretty_print(ediff_alph, "ediff_alph");
        if (nden == 2)
            pretty_print(ediff_beta, "ediff_beta");
    }

    if (cfg.get_param<bool>("_mask_ediff_mo")) {
        arma::mat ediff_masked_alph;
        arma::mat ediff_masked_beta;
        make_masked_mat(ediff_masked_alph, ediff_alph, indices_mo_alph, 0.0, true);
        ediff_alph = ediff_masked_alph;
        if (nden == 2) {
            make_masked_mat(ediff_masked_beta, ediff_beta, indices_mo_beta, 0.0, true);
            ediff_beta = ediff_masked_beta;
        }
        if (print_level >= 10) {
            pretty_print(ediff_alph, "ediff_alph (masked)");
            if (nden == 2)
                pretty_print(ediff_beta, "ediff_beta (masked)");
        }
    }

    const int save_level = cfg.get_param<int>("save");
    std::string prefix;
    if (cfg.has_param("prefix"))
        prefix = cfg.get_param("prefix");
    else
        prefix = "";
    if (save_level > 0) {
        ediff_alph.save(prefix + "ediff_alph.dat", arma::arma_ascii);
        if (nden == 2)
            ediff_beta.save(prefix + "ediff_beta.dat", arma::arma_ascii);
    }

    // Transform the property vector and gradient vector/RHS from the
    // AO basis to the occ-virt MO basis, and repack the gradient
    // vector/RHS from a matrix into a vector, where 'a' in {ia} is
    // the fast index.
    // This is a matrix because an operator may have multiple
    // components, each a vector.
    for (size_t i = 0; i < operators.size(); i++) {
        operators[i].init_indices(fragment_occupations, cfg);
        operators[i].form_rhs(C, occupations, cfg);
    }

    const int read_level = cfg.get_param<int>("read");
    // Only keep the response vectors for one frequency in memory at a
    // time, so these aren't vectors of cubes.
    for (size_t i = 0; i < operators.size(); i++) {
        if (operators.at(i).do_response) {
            if (read_level == 1) {
                // read in MO basis
                operators.at(i).rspvecs_alph.load("rspvecs_" + operators[i].metadata.operator_label + "_mo_alph.dat", arma::arma_ascii);
            } else if (read_level == 2) {
                // read in AO basis, need to transform to MO basis
                arma::cube tmp_ao_alph;
                tmp_ao_alph.load("rspvecs_" + operators[i].metadata.operator_label + "_ao_alph.dat", arma::arma_ascii);
                one_electron_mn_mats_to_ia_vecs(operators.at(i).rspvecs_alph, tmp_ao_alph, C_occ_alph, C_virt_alph);
            } else {
                // Space has already been allocated, don't need to
                // do anything.
            }
            if (nden == 2) {
                if (read_level == 1) {
                    // read in MO basis
                    operators.at(i).rspvecs_beta.load("rspvecs_" + operators[i].metadata.operator_label + "_mo_beta.dat", arma::arma_ascii);
                } else if (read_level == 2) {
                    // read in AO basis, need to transform to MO basis
                    arma::cube tmp_ao_beta;
                    tmp_ao_beta.load("rspvecs_" + operators[i].metadata.operator_label + "_ao_beta.dat", arma::arma_ascii);
                    one_electron_mn_mats_to_ia_vecs(operators.at(i).rspvecs_beta, tmp_ao_beta, C_occ_beta, C_virt_beta);
                } else {
                    // Space has already been allocated, don't need to
                    // do anything.
                }
            }
        }
    }

    solver_iterator->set_orbital_occupations(
        nocc_alph, nvirt_alph, nocc_beta, nvirt_beta
        );

    solver_iterator->set_fragment_occupations(fragment_occupations);

    for (size_t f = 0; f < omega.size(); f++) {

        const double frequency = omega[f];

        // Keep results for this frequency so they can be printed on
        // each iteration.
        arma::cube results_freq(tot_n_slices, tot_n_slices, nden);

        for (size_t i = 0; i < operators.size(); i++) {

            // The initial guess for the response vectors is the
            // uncoupled result. If response vectors were read in from
            // disk, then they serve as the guess, which should not be
            // formed.
            if (read_level == 0) {
                operators.at(i).form_guess_rspvec(ediff_alph, frequency, false, nov_alph, cfg);
                if (nden == 2)
                    operators.at(i).form_guess_rspvec(ediff_beta, frequency, true, nov_beta, cfg);
                // Save the initial response vector guess to disk if
                // requested.  TODO change file names if using masked
                // quantities?
                operators.at(i).save_to_disk(save_level, true);
            }
        }

        // Print the uncoupled result (initial guess).
        const bool mask_form_results_mo = cfg.get_param<bool>("_mask_form_results_mo");
        if (print_level >= 1) {
            if (mask_form_results_mo)
                form_results(results_freq, operators, &indices_mo);
            else
                form_results(results_freq, operators);
            arma::mat results_freq_mat = results_freq.slice(0);
            if (nden == 2) {
                results_freq_mat += results_freq.slice(1);
                results_freq *= 2.0;
            }
            std::cout << " " << dashes << std::endl;
            std::cout << "  Uncoupled result (initial guess):" << std::endl;
            print_results_with_labels(
                results_freq_mat, operator_labels, component_labels);
        }

        // Initialize the solver.
        solver_iterator->init(
            &operators,
            const_cast<configurable *>(&cfg),
            matvec,
            const_cast<arma::cube *>(&C),
            &ediff_alph, &ediff_beta,
            frequency, maxiter, conv
            );

        // Run the solver.
        solver_iterator->run();

        // Form the final linear response values by dotting the
        // response vector(s) with the property vector(s).  The
        // property vectors are the same as the input gradient
        // vectors.
        if (mask_form_results_mo)
            form_results(results_freq, operators, &indices_mo);
        else
            form_results(results_freq, operators);
        results_alph.slice(f) = results_freq.slice(0);
        if (nden == 2)
            results_beta.slice(f) = results_freq.slice(1);

        // Save the RHS and response vectors to disk if requested.
        for (size_t i = 0; i < operators.size(); i++)
            operators[i].save_to_disk(save_level, false);
    }

    if (nden == 1) {
        results = results_alph;
    }
    if (nden == 2) {
        results = 2 * (results_alph + results_beta);
    }

    if (print_level >= 1) {
        std::cout << "  Final result: " << std::endl;
        print_results_with_labels(
            results, operator_labels, component_labels);
    }

    return;

}

} // namespace libresponse
