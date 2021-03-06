/*
  bgmg - tool to calculate log likelihood of BGMG and UGMG mixture models
  Copyright (C) 2018 Oleksandr Frei 

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <stdint.h>
#define DLL_PUBLIC

DLL_PUBLIC const char* bgmg_get_last_error();
DLL_PUBLIC void bgmg_init_log(const char* file);
DLL_PUBLIC void bgmg_log_message(const char* message);
DLL_PUBLIC int64_t bgmg_dispose(int context_id);
DLL_PUBLIC const char* bgmg_status(int context_id);
DLL_PUBLIC int64_t bgmg_init(int context_id, const char* bim_file, const char* frq_file, const char* chr_labels, const char* trait1_file, const char* trait2_file, const char* exclude, const char* extract);
DLL_PUBLIC int64_t bgmg_convert_plink_ld(int context_id, const char* plink_ld_gz, const char* plink_ld_bin);
DLL_PUBLIC int64_t bgmg_set_tag_indices(int context_id, int num_snp, int num_tag, int* tag_indices);
DLL_PUBLIC int64_t bgmg_get_num_tag(int context_id);
DLL_PUBLIC int64_t bgmg_get_num_snp(int context_id);
DLL_PUBLIC int64_t bgmg_retrieve_tag_indices(int context_id, int num_tag, int* tag_indices);
DLL_PUBLIC int64_t bgmg_set_chrnumvec(int context_id, int length, int* values);
DLL_PUBLIC int64_t bgmg_retrieve_chrnumvec(int context_id, int length, int* buffer);
DLL_PUBLIC int64_t bgmg_set_option(int context_id, char* option, double value);
DLL_PUBLIC int64_t bgmg_set_zvec(int context_id, int trait, int length, float* values);
DLL_PUBLIC int64_t bgmg_set_nvec(int context_id, int trait, int length, float* values);
DLL_PUBLIC int64_t bgmg_set_mafvec(int context_id, int length, float* values);
DLL_PUBLIC int64_t bgmg_retrieve_zvec(int context_id, int trait, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_retrieve_nvec(int context_id, int trait, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_retrieve_mafvec(int context_id, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_set_ld_r2_coo(int context_id, int64_t length, int* snp_index, int* tag_index, float* r2);
DLL_PUBLIC int64_t bgmg_set_ld_r2_coo_from_file(int context_id, const char* filename);
DLL_PUBLIC int64_t bgmg_set_ld_r2_csr(int context_id, int chr_label);
DLL_PUBLIC int64_t bgmg_num_ld_r2_snp(int context_id, int snp_index);
DLL_PUBLIC int64_t bgmg_retrieve_ld_r2_snp(int context_id, int snp_index, int length, int* tag_index, float* r2);
DLL_PUBLIC int64_t bgmg_num_ld_r2_chr(int context_id, int chr_label);
DLL_PUBLIC int64_t bgmg_retrieve_ld_r2_chr(int context_id, int chr_label, int length, int* snp_index, int* tag_index, float* r2);
DLL_PUBLIC int64_t bgmg_set_weights(int context_id, int length, float* values);
DLL_PUBLIC int64_t bgmg_set_weights_randprune(int context_id, int n, float r2);
DLL_PUBLIC int64_t bgmg_perform_ld_clump(int context_id, float r2, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_retrieve_weights(int context_id, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_retrieve_tag_r2_sum(int context_id, int component_id, float num_causal, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_retrieve_ld_tag_r2_sum(int context_id, int length, float* buffer);  // LD scores (r2 and r4)
DLL_PUBLIC int64_t bgmg_retrieve_ld_tag_r4_sum(int context_id, int length, float* buffer);
DLL_PUBLIC int64_t bgmg_retrieve_weighted_causal_r2(int context_id, int length, float* buffer);
DLL_PUBLIC double bgmg_calc_univariate_cost(int context_id, int trait_index, double pi_vec, double sig2_zero, double sig2_beta);
DLL_PUBLIC double bgmg_calc_univariate_cost_with_deriv(int context_id, int trait_index, double pi_vec, double sig2_zero, double sig2_beta, int deriv_length, double* deriv);
DLL_PUBLIC int64_t bgmg_calc_univariate_pdf(int context_id, int trait_index, float pi_vec, float sig2_zero, float sig2_beta, int length, float* zvec, float* pdf);
DLL_PUBLIC int64_t bgmg_calc_univariate_power(int context_id, int trait_index, float pi_vec, float sig2_zero, float sig2_beta, float zthresh, int length, float* nvec, float* svec);
DLL_PUBLIC int64_t bgmg_calc_univariate_delta_posterior(int context_id, int trait_index, float pi_vec, float sig2_zero, float sig2_beta, int length, float* c0, float* c1, float* c2);
DLL_PUBLIC double bgmg_calc_bivariate_cost(int context_id, int pi_vec_len, float* pi_vec, int sig2_beta_len, float* sig2_beta, float rho_beta, int sig2_zero_len, float* sig2_zero, float rho_zero);
DLL_PUBLIC int64_t bgmg_calc_bivariate_pdf(int context_id, int pi_vec_len, float* pi_vec, int sig2_beta_len, float* sig2_beta, float rho_beta, int sig2_zero_len, float* sig2_zero, float rho_zero, int length, float* zvec1, float* zvec2, float* pdf);
DLL_PUBLIC int64_t bgmg_clear_loglike_cache(int context_id);
DLL_PUBLIC int64_t bgmg_get_loglike_cache_size(int context_id);
DLL_PUBLIC int64_t bgmg_get_loglike_cache_univariate_entry(int context_id, int entry_index, float* pi_vec, float* sig2_zero, float* sig2_beta, double* cost);
DLL_PUBLIC int64_t bgmg_get_loglike_cache_bivariate_entry(int context_id, int entry_index, int pi_vec_len, float* pi_vec, int sig2_beta_len, float* sig2_beta, float* rho_beta, int sig2_zero_len, float* sig2_zero, float* rho_zero, double* cost);
