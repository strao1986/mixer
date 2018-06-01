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

#include "bgmg_calculator.h"

#include <assert.h>

#include <chrono>
#include <random>
#include <limits>
#include <algorithm>
#include <vector>
#include <valarray>
#include <cmath>
#include <sstream>
#include <set>

#include "boost/throw_exception.hpp"

#include "bgmg_log.h"

#define OMP_CHUNK 1000

void BgmgCalculator::check_num_snp(int length) {
  if (num_snp_ == -1) BOOST_THROW_EXCEPTION(::std::runtime_error("call set_tag_indices first"));
  if (num_snp_ != length) BOOST_THROW_EXCEPTION(::std::runtime_error("length != num_snps_"));
}

void BgmgCalculator::check_num_tag(int length) {
  if (num_tag_ == -1) BOOST_THROW_EXCEPTION(::std::runtime_error("call set_tag_indices first"));
  if (num_tag_ != length) BOOST_THROW_EXCEPTION(::std::runtime_error("length != num_snps_"));
}

int64_t BgmgCalculator::set_zvec(int trait, int length, float* values) {
  if ((trait != 1) && (trait != 2)) BOOST_THROW_EXCEPTION(::std::runtime_error("trait must be 1 or 2"));

  int num_undef = 0;
  for (int i = 0; i < length; i++) if (!std::isfinite(values[i])) num_undef++;
  LOG << " set_zvec(trait=" << trait << "); num_undef=" << num_undef;

  check_num_tag(length);
  if (trait == 1) {
    zvec1_.assign(values, values + length);
  } else {
    zvec2_.assign(values, values + length);
  }
  return 0;
}

int64_t BgmgCalculator::set_nvec(int trait, int length, float* values) {
  if ((trait != 1) && (trait != 2)) BOOST_THROW_EXCEPTION(::std::runtime_error("trait must be 1 or 2"));
  for (int i = 0; i < length; i++) {
    if (!std::isfinite(values[i])) BOOST_THROW_EXCEPTION(::std::runtime_error("encounter undefined values"));
  }

  LOG << " set_nvec(trait=" << trait << "); ";
  check_num_tag(length);
  if (trait == 1) {
    nvec1_.assign(values, values + length);
  } else {
    nvec2_.assign(values, values + length);
  }

  return 0;
}


int64_t BgmgCalculator::set_weights(int length, float* values) {
  for (int i = 0; i < length; i++) {
    if (!std::isfinite(values[i])) BOOST_THROW_EXCEPTION(::std::runtime_error("encounter undefined values"));
  }

  LOG << " set_weights; ";
  check_num_tag(length);
  weights_.assign(values, values+length);
  return 0;
}

int64_t BgmgCalculator::set_option(char* option, double value) {
  LOG << " set_option(" << option << "=" << value << "); ";

  if (!strcmp(option, "diag")) {
    log_disgnostics(); return 0;
  } else if (!strcmp(option, "kmax")) {
    clear_state(); k_max_ = static_cast<int>(value); return 0;
  } else if (!strcmp(option, "r2min")) {
    clear_state(); r2_min_ = value; return 0;
  } else if (!strcmp(option, "max_causals")) {
    if (!last_num_causals_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("can't change max_causals after find_snp_order"));
    clear_state(); max_causals_ = static_cast<int>(value); return 0;
  } else if (!strcmp(option, "num_components")) {
    if (!last_num_causals_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("can't change num_components after find_snp_order"));
    clear_state(); num_components_ = static_cast<int>(value); return 0;
  } else if (!strcmp(option, "fast_cost")) {
    use_fast_cost_calc_ = (value != 0); return 0;
  }

  BOOST_THROW_EXCEPTION(::std::runtime_error("unknown option"));
  return 0;
}

#define CHECK_SNP_INDEX(i) if (i < 0 || i >= num_snp_) BOOST_THROW_EXCEPTION(::std::runtime_error("CHECK_SNP_INDEX failed"));
#define CHECK_TAG_INDEX(i) if (i < 0 || i >= num_tag_) BOOST_THROW_EXCEPTION(::std::runtime_error("CHECK_TAG_INDEX failed"));

int64_t BgmgCalculator::set_tag_indices(int num_snp, int num_tag, int* tag_indices) {
  if (num_snp_ != -1 || num_tag_ != -1) BOOST_THROW_EXCEPTION(::std::runtime_error("can not call set_tag_indices twice"));

  LOG << " set_tag_indices(num_snp=" << num_snp << ", num_tag=" << num_tag << "); ";
  num_snp_ = num_snp;
  num_tag_ = num_tag;

  is_tag_.resize(num_snp, 0);
  snp_to_tag_.resize(num_snp, -1);
  tag_to_snp_.assign(tag_indices, tag_indices + num_tag);
  for (int i = 0; i < tag_to_snp_.size(); i++) {
    CHECK_SNP_INDEX(tag_to_snp_[i]);
    is_tag_[tag_to_snp_[i]] = 1;
    snp_to_tag_[tag_to_snp_[i]] = i;
  }
  return 0;
}

// A timer that fire an event each X milliseconds.
class SimpleTimer {
public:
  SimpleTimer(int period_ms) : start_(std::chrono::system_clock::now()), period_ms_(period_ms) {}

  int elapsed_ms() {
    auto delta = (std::chrono::system_clock::now() - start_);
    auto delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
    return delta_ms.count();
  }

  bool fire() {
    if (elapsed_ms() < period_ms_) return false;
    start_ = std::chrono::system_clock::now();
    return true;
  }
private:
  std::chrono::time_point<std::chrono::system_clock> start_;
  int period_ms_;
};


int64_t BgmgCalculator::set_ld_r2_coo(int length, int* snp_index, int* tag_index, float* r2) {
  if (!csr_ld_r2_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("can't call set_ld_r2_coo after set_ld_r2_csr"));
  LOG << ">set_ld_r2_coo(length=" << length << "); ";

  if (last_num_causals_.empty()) find_snp_order();

  for (int i = 0; i < length; i++) {
    if (!std::isfinite(r2[i])) BOOST_THROW_EXCEPTION(::std::runtime_error("encounter undefined values"));
  }

  SimpleTimer timer(-1);

  int was = coo_ld_.size();
  for (int i = 0; i < length; i++) {
    CHECK_SNP_INDEX(snp_index[i]); CHECK_SNP_INDEX(tag_index[i]);
    if (r2[i] < r2_min_) continue;
    // tricky part here is that we take into account snp_can_be_causal_
    // there is no reason to keep LD information about certain causal SNP if we never selecting it as causal
    // (see how snp_can_be_causal_ is created during find_snp_order() call)
    if (snp_can_be_causal_[snp_index[i]] && is_tag_[tag_index[i]]) coo_ld_.push_back(std::make_tuple(snp_index[i], snp_to_tag_[tag_index[i]], r2[i]));
    if (snp_can_be_causal_[tag_index[i]] && is_tag_[snp_index[i]]) coo_ld_.push_back(std::make_tuple(tag_index[i], snp_to_tag_[snp_index[i]], r2[i]));
  }
  LOG << "<set_ld_r2_coo: done; coo_ld_.size()=" << coo_ld_.size() << " (new: " << coo_ld_.size() - was << "), elapsed time " << timer.elapsed_ms() << " ms";
  return 0;
}

int64_t BgmgCalculator::set_ld_r2_csr() {
  if (coo_ld_.empty()) 
    BOOST_THROW_EXCEPTION(::std::runtime_error("coo_ld_ is empty"));

  LOG << ">set_ld_r2_csr (coo_ld_.size()==" << coo_ld_.size() << "); ";

  SimpleTimer timer(-1);

  LOG << " set_ld_r2_csr adds " << tag_to_snp_.size() << " elements with r2=1.0 to the diagonal of LD r2 matrix";
  for (int i = 0; i < tag_to_snp_.size(); i++)
    coo_ld_.push_back(std::make_tuple(tag_to_snp_[i], i, 1.0f));
  
  // Use parallel sort? https://software.intel.com/en-us/articles/a-parallel-stable-sort-using-c11-for-tbb-cilk-plus-and-openmp
  std::sort(coo_ld_.begin(), coo_ld_.end());

  csr_ld_tag_index_.reserve(coo_ld_.size());
  csr_ld_r2_.reserve(coo_ld_.size());

  for (int i = 0; i < coo_ld_.size(); i++) {
    csr_ld_tag_index_.push_back(std::get<1>(coo_ld_[i]));
    csr_ld_r2_.push_back(std::get<2>(coo_ld_[i]));
  }

  // find starting position for each snp
  csr_ld_snp_index_.resize(snp_to_tag_.size() + 1, coo_ld_.size());
  for (int i = (coo_ld_.size() - 1); i >= 0; i--) {
    int snp_index = std::get<0>(coo_ld_[i]);
    csr_ld_snp_index_[snp_index] = i;
  }

  for (int i = (csr_ld_snp_index_.size() - 2); i >= 0; i--)
    if (csr_ld_snp_index_[i] > csr_ld_snp_index_[i + 1])
      csr_ld_snp_index_[i] = csr_ld_snp_index_[i + 1];

  LOG << "<set_ld_r2_csr (coo_ld_.size()==" << coo_ld_.size() << "); elapsed time " << timer.elapsed_ms() << " ms"; 
  coo_ld_.clear();
  return 0;
}

class xorshf96  //period 2^96-1
{
public:
  using result_type = unsigned long;
  static constexpr result_type min() { return 0; }
  static constexpr result_type max() { return std::numeric_limits<unsigned long>::max(); }
  result_type operator()() {
    static unsigned long x = 123456789, y = 362436069, z = 521288629;
    unsigned long t;
    x ^= x << 16;
    x ^= x >> 5;
    x ^= x << 1;

    t = x;
    x = y;
    y = z;
    z = t ^ x ^ y;

    return z;
  }
};

int64_t BgmgCalculator::find_snp_order() {
  if (max_causals_ <= 0 || max_causals_ > num_snp_) BOOST_THROW_EXCEPTION(::std::runtime_error("find_snp_order: max_causals_ <= 0 || max_causals_ > num_snp_"));
  if (num_components_ <= 0 || num_components_ > 3) BOOST_THROW_EXCEPTION(::std::runtime_error("find_snp_order: num_components_ must be between 1 and 3"));
  if (last_num_causals_.size() > 0) BOOST_THROW_EXCEPTION(::std::runtime_error("find_snp_order: called twice"));

  LOG << ">find_snp_order(num_components_=" << num_components_ << ", k_max_=" << k_max_ << ", max_causals_=" << max_causals_ << ")";

  SimpleTimer timer(-1);

  snp_can_be_causal_.resize(num_snp_, 0);

  xorshf96 random_engine;
  std::vector<int> perm(num_snp_, 0);

  SimpleTimer log_timer(10000); // log some message each 10 seconds
  for (int component_index = 0; component_index < num_components_; component_index++) {
    snp_order_.push_back(std::make_shared<DenseMatrix<int>>(max_causals_, k_max_));
    tag_r2sum_.push_back(std::make_shared<DenseMatrix<float>>(num_tag_, k_max_));
    
    tag_r2sum_[component_index]->InitializeZeros();
    last_num_causals_.push_back(0);
    
    for (int k = 0; k < k_max_; k++) {
      if (log_timer.fire())
        LOG << " find_snp_order still working, component_id=" << component_index << ", k=" << k;

      for (int i = 0; i < num_snp_; i++) perm[i] = i;
      
      // perform partial Fisher Yates shuffle (must faster than full std::shuffle)
      // swap_offset is a random integer, with max of n-1, n-2, n-3, ..., n-max_causals
      for (int i = 0; i < max_causals_; i++) {
        const int swap_offset = std::uniform_int_distribution<int>(0, num_snp_ - i - 1)(random_engine);
        std::iter_swap(perm.begin() + i, perm.begin() + i + swap_offset);
      }

      for (int i = 0; i < max_causals_; i++) {
        (*snp_order_[component_index])(i, k) = perm[i];
        snp_can_be_causal_[perm[i]] = 1;
      }
    }
  }

  int num_can_be_causal = 0;
  for (int i = 0; i < num_snp_; i++) num_can_be_causal += snp_can_be_causal_[i];
  LOG << "<find_snp_order: num_can_be_causal = " << num_can_be_causal << ", elapsed time " << timer.elapsed_ms() << "ms";
  return 0;
}

int64_t BgmgCalculator::find_tag_r2sum(int component_id, float num_causals) {
  if (num_causals < 0 || num_causals >= max_causals_) BOOST_THROW_EXCEPTION(::std::runtime_error("find_tag_r2sum: num_causals < 0 || num_causals >= max_causals_"));
  if (component_id < 0 || component_id >= num_components_) BOOST_THROW_EXCEPTION(::std::runtime_error("find_tag_r2sum: component_id must be between 0 and num_components_"));

  const float num_causals_original = num_causals;
  if (last_num_causals_.empty()) find_snp_order();

  float last_num_causals = last_num_causals_[component_id]; 
  
  LOG << ">find_tag_r2sum(component_id=" << component_id << ", num_causals=" << num_causals << ", last_num_causals=" << last_num_causals << ")";

  // if num_causal is more than twice lower than last_num_causals we should re-calculate tag_r2sum from scratch.
  if (num_causals < (last_num_causals / 2)) {
    clear_tag_r2sum(component_id);
    last_num_causals = 0.0f;
  }

  SimpleTimer timer(-1);

  // changeset contains a list of indices with corresponding weight
  // indices apply to snp_order_[component_id] array.
  // weights are typicaly +1 (to increase by r2) or -1 (to decrease by r2).
  // First and last weights is float-point number between 1 and -1,
  // to handle cases when num_causals is float-point number (derived from pivec).
  // This is important for fminsearch which get's confused if cost is a stepwise of pivec.
  std::vector<std::pair<int, float>> changeset;
  
  // Decreasing number of causals from B to A has an opposite effect to increasing from A to B.
  // To handle decreasing case we just swap num_causals and last_num_causals, and set sign to -1.0f.
  float sign = 1.0f;
  if (num_causals < last_num_causals) {
    float tmp = num_causals; num_causals = last_num_causals; last_num_causals = tmp;
    sign = -1.0f;
  }

  // There are 3 cases
  // 1. floor(num_causals) == floor(last_num_causals)
  // 2. floor(num_causals) == floor(last_num_causals) + 1
  // 3. floor(num_causals) >= floor(last_num_causals) + 2

  float floor_num_causals = floor(num_causals);
  float floor_last_num_causals = floor(last_num_causals);
  if ((int)floor_num_causals == (int)floor_last_num_causals) {
    changeset.push_back(std::make_pair((int)floor_last_num_causals, sign * (num_causals - last_num_causals)));
  }
  else if ((int)floor_num_causals >= ((int)floor_last_num_causals + 1)) {
    // handle case 2 and case 3 - lower boundary
    changeset.push_back(std::make_pair((int)floor_last_num_causals, sign * (floor_last_num_causals + 1.0f - last_num_causals)));

    // happends for the case 3 - bulk change (empty loop in case 2)
    for (int i = ((int)floor_last_num_causals + 1); i < (int)floor_num_causals; i++) {
      changeset.push_back(std::make_pair(i, sign));
    }

    // handle case 2 and case 3 - upper boundary
    changeset.push_back(std::make_pair((int)floor_num_causals, sign * (num_causals - floor_num_causals)));
  }
  else {
    BOOST_THROW_EXCEPTION(::std::runtime_error("floor_num_causals < floor_last_num_causals"));
  }

  // it is OK to parallelize the following loop on k_index, because:
  // - all structures here are readonly, except tag_r2sum_ that we are accumulating
  // - two threads will never touch the same memory location (that's why we choose k_index as an outer loop)
#pragma omp parallel for schedule(static)
  for (int k_index = 0; k_index < k_max_; k_index++) {
    for (auto change : changeset) {
      int scan_index = change.first;
      float scan_weight = change.second;
      int snp_index = (*snp_order_[component_id])(scan_index, k_index);
      int r2_index_from = csr_ld_snp_index_[snp_index];
      int r2_index_to = csr_ld_snp_index_[snp_index + 1];
      for (int r2_index = r2_index_from; r2_index < r2_index_to; r2_index++) {
        int tag_index = csr_ld_tag_index_[r2_index];
        float r2 = csr_ld_r2_[r2_index];
        (*tag_r2sum_[component_id])(tag_index, k_index) += (scan_weight * r2);
      }
    }
  }

  LOG << "<find_tag_r2sum(component_id=" << component_id << ", num_causals=" << num_causals_original << ", last_num_causals=" << last_num_causals << "), elapsed time " << timer.elapsed_ms() << "ms";

  last_num_causals_[component_id] = num_causals_original;
  return 0;
}

int64_t BgmgCalculator::set_hvec(int length, float* values) {
  for (int i = 0; i < length; i++) {
    if (!std::isfinite(values[i])) BOOST_THROW_EXCEPTION(::std::runtime_error("encounter undefined values"));
  }

  if (!hvec_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("can not set hvec twice"));

  LOG << ">set_hvec(" << length << "); ";
  check_num_snp(length);
  hvec_.assign(values, values + length);

  for (int i = 0; i < csr_ld_r2_.size(); i++) {
    int tag = csr_ld_tag_index_[i];
    int snp = tag_to_snp_[tag];
    csr_ld_r2_[i] *= values[snp];
  }

  LOG << "<set_hvec(" << length << "); ";
  return 0;
}


int64_t BgmgCalculator::retrieve_tag_r2_sum(int component_id, float num_causal, int length, float* buffer) {
  if (length != (k_max_ * num_tag_)) BOOST_THROW_EXCEPTION(::std::runtime_error("wrong buffer size"));
  if (component_id < 0 || component_id >= num_components_ || tag_r2sum_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("wrong component_id"));

  LOG << " retrieve_tag_r2_sum(component_id=" << component_id << ", num_causal=" << num_causal << ")";

  // use negative to retrieve tag_r2_sum for last_num_causal (for debugging purpose)
  if (num_causal >= 0) {
    find_tag_r2sum(component_id, num_causal);
  }

  for (int tag_index = 0; tag_index < num_tag_; tag_index++) {
    for (int k_index = 0; k_index < k_max_; k_index++) {
      float tag_r2sum = (*tag_r2sum_[component_id])(tag_index, k_index);
      buffer[tag_index * k_max_ + k_index] = tag_r2sum;
    }
  }

  return 0;
}

// pdf of gaussian (normal) distribution with 0 mean and std error of s
// z is the point where to calculate pdf
inline float gaussian_pdf_float(const float z, const float s) {
  static const float inv_sqrt_2pi = 0.3989422804014327f;
  const float a = z / s;
  const float pdf = inv_sqrt_2pi / s * std::exp(-0.5 * a * a);
  return pdf;
}

inline double gaussian_pdf_double(const double z, const double s) {
  static const double inv_sqrt_2pi = 0.3989422804014327;
  const double a = z / s;
  const double pdf = inv_sqrt_2pi / s * std::exp(-0.5 * a * a);
  return pdf;
}

inline double gaussian2_pdf_double(const double z1, const double z2, const double a11, const double a12, const double a22) {
  static const double pi_const = 3.14159265358979323846;
  static const double log_pi = -1.0 * log(2 * pi_const);

  // Calculation of log - likelihood and pdf, specific to bivariate normal
  // distribution with zero mean.It takes into account an explicit formula
  // for inverse 2x2 matrix, S = [a b; c d], => S^-1 = [d - b; -c a] . / det(S)
  double dt = a11 * a22 - a12 * a12;  // det(S)

  const double log_exp = -0.5 * (a22*z1*z1 + a11*z2*z2 - 2.0*a12*z1*z2) / dt;
  const double log_dt = -0.5 * log(dt);

  const double pdf = exp(log_pi + log_dt + log_exp);
  return pdf;
}

int64_t BgmgCalculator::calc_univariate_pdf(float pi_vec, float sig2_zero, float sig2_beta, int length, float* zvec, float* pdf) {
  // input buffer "zvec" contains z scores (presumably an equally spaced grid)
  // output buffer contains pdf(z), aggregated across all SNPs with corresponding weights
  if (nvec1_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("nvec1 is not set"));
  if (weights_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("weights are not set"));

  float num_causals = pi_vec * static_cast<float>(num_snp_);
  if ((int)num_causals >= max_causals_) BOOST_THROW_EXCEPTION(::std::runtime_error("too large values in pi_vec"));
  const int component_id = 0;   // univariate is always component 0.

  LOG << ">calc_univariate_pdf(pi_vec=" << pi_vec << ", sig2_zero=" << sig2_zero << ", sig2_beta=" << sig2_beta << ")";

  find_tag_r2sum(component_id, num_causals);

  SimpleTimer timer(-1);

  const float pi_k = 1. / static_cast<float>(k_max_);

// omp reduction on std::vector ( https://stackoverflow.com/questions/43168661/openmp-and-reduction-on-stdvector ) - did not work for microsoft compiler
// #pragma omp declare reduction(vec_double_plus : std::vector<double> : \
//                               std::transform(omp_out.begin(), omp_out.end(), omp_in.begin(), omp_out.begin(), std::plus<double>())) \
//                     initializer(omp_priv = omp_orig)
// Final solution is to do a the reduction with omp critical (see here http://pages.tacc.utexas.edu/~eijkhout/pcse/html/omp-reduction.html )

  // we accumulate crazy many small values - each of them is OK as float; the sum is also OK as float;  
  // but accumulation must be done with double precision.
  // std::vector<double> pdf_double(length, 0.0);
  std::valarray<double> pdf_double(0.0, length);

#pragma omp parallel
  {
    std::valarray<double> pdf_double_local(0.0, length);
#pragma omp for schedule(static)
    for (int tag_index = 0; tag_index < num_tag_; tag_index++) {
      if (weights_[tag_index] == 0) continue;
      for (int k_index = 0; k_index < k_max_; k_index++) {
        float tag_r2sum = (*tag_r2sum_[component_id])(tag_index, k_index);
        float sig2eff = tag_r2sum * nvec1_[tag_index] * sig2_beta + sig2_zero;
        float s = sqrt(sig2eff);

        for (int z_index = 0; z_index < length; z_index++) {
          float pdf_tmp = pi_k * gaussian_pdf_float(zvec[z_index], s);
          pdf_double_local[z_index] += static_cast<double>(pdf_tmp * weights_[tag_index]);
        }
      }
    }
#pragma omp critical
    pdf_double += pdf_double_local;
  }

  for (int i = 0; i < length; i++) pdf[i] = static_cast<float>(pdf_double[i]);
  LOG << "<calc_univariate_pdf(pi_vec=" << pi_vec << ", sig2_zero=" << sig2_zero << ", sig2_beta=" << sig2_beta << "), elapsed time " << timer.elapsed_ms() << "ms";
  return 0;
}

double BgmgCalculator::calc_univariate_cost(float pi_vec, float sig2_zero, float sig2_beta) {
  if (zvec1_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("zvec1 is not set"));
  if (nvec1_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("nvec1 is not set"));
  if (weights_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("weights are not set"));

  if (use_fast_cost_calc_) return calc_univariate_cost_fast(pi_vec, sig2_zero, sig2_beta);

  float num_causals = pi_vec * static_cast<float>(num_snp_);
  if ((int)num_causals >= max_causals_) return 1e100; // too large pi_vec
  const int component_id = 0;   // univariate is always component 0.
    
  LOG << ">calc_univariate_cost(pi_vec=" << pi_vec << ", sig2_zero=" << sig2_zero << ", sig2_beta=" << sig2_beta << ")";
  find_tag_r2sum(component_id, num_causals);

  SimpleTimer timer(-1);

  const double pi_k = 1. / static_cast<float>(k_max_);
  
  double log_pdf_total = 0.0;

#pragma omp parallel for schedule(static) reduction(+: log_pdf_total)
  for (int tag_index = 0; tag_index < num_tag_; tag_index++) {
    if (weights_[tag_index] == 0) continue;
    if (!std::isfinite(zvec1_[tag_index])) continue;

    double pdf_tag = 0.0f;
    for (int k_index = 0; k_index < k_max_; k_index++) {
      double tag_r2sum = (*tag_r2sum_[component_id])(tag_index, k_index);
      double sig2eff = tag_r2sum * nvec1_[tag_index] * sig2_beta + static_cast<double>(sig2_zero);

      double s = sqrt(sig2eff);
      double pdf = pi_k * gaussian_pdf_double(zvec1_[tag_index], s);
      pdf_tag += pdf;
    }
    log_pdf_total += -log(pdf_tag) * weights_[tag_index];
  }

  LOG << "<calc_univariate_cost(pi_vec=" << pi_vec << ", sig2_zero=" << sig2_zero << ", sig2_beta=" << sig2_beta << "), cost=" << log_pdf_total << ", elapsed time " << timer.elapsed_ms() << "ms";
  return log_pdf_total;
}

std::string calc_bivariate_cost_params_to_str(int pi_vec_len, float* pi_vec, int sig2_beta_len, float* sig2_beta, float rho_beta, int sig2_zero_len, float* sig2_zero, float rho_zero) {
  std::stringstream ss;
  ss << "pi_vec=[" << pi_vec[0] << ", " << pi_vec[1] << ", " << pi_vec[2] << "], "
     << "sig2_beta=[" << sig2_beta[0] << ", " << sig2_beta[1] << "], "
     << "rho_beta=" << rho_beta << ", "
     << "sig2_zero=[" << sig2_zero[0] << ", " << sig2_zero[1] << "], "
     << "rho_zero=" << rho_zero;
  return ss.str();
}

double BgmgCalculator::calc_bivariate_cost(int pi_vec_len, float* pi_vec, int sig2_beta_len, float* sig2_beta, float rho_beta, int sig2_zero_len, float* sig2_zero, float rho_zero) {
  if (zvec1_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("zvec1 is not set"));
  if (nvec1_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("nvec1 is not set"));
  if (zvec2_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("zvec2 is not set"));
  if (nvec2_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("nvec2 is not set"));
  if (weights_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("weights are not set"));
  if (num_components_ != 3) BOOST_THROW_EXCEPTION(::std::runtime_error("calc_bivariate_cost: require num_components == 3. Remember to call set_option('num_components', 3)."));
  if (sig2_beta_len != 2) BOOST_THROW_EXCEPTION(::std::runtime_error("calc_bivariate_cost: sig2_beta_len != 2"));
  if (sig2_zero_len != 2) BOOST_THROW_EXCEPTION(::std::runtime_error("calc_bivariate_cost: sig2_zero_len != 2"));
  if (pi_vec_len != 3) BOOST_THROW_EXCEPTION(::std::runtime_error("calc_bivariate_cost: pi_vec_len != 3"));

  if (use_fast_cost_calc_) return calc_bivariate_cost_fast(pi_vec_len, pi_vec, sig2_beta_len, sig2_beta, rho_beta, sig2_zero_len, sig2_zero, rho_zero);

  std::string ss = calc_bivariate_cost_params_to_str(pi_vec_len, pi_vec, sig2_beta_len, sig2_beta, rho_beta, sig2_zero_len, sig2_zero, rho_zero);
  LOG << ">calc_bivariate_cost(" << ss << ")";

  float num_causals[3];
  for (int component_id = 0; component_id < 3; component_id++) {
    num_causals[component_id] = pi_vec[component_id] * static_cast<float>(num_snp_);
    if ((int)num_causals[component_id] >= max_causals_) return 1e100; // too large pi_vec
  }

  for (int component_id = 0; component_id < 3; component_id++) {
    find_tag_r2sum(component_id, num_causals[component_id]);
  }

  SimpleTimer timer(-1);

  // Sigma0  = [a0 b0; b0 c0];
  const double a0 = sig2_zero[0];
  const double c0 = sig2_zero[1];
  const double b0 = sqrt(a0 * c0) * rho_zero;

  // pi_k is mixture weight
  const double pi_k = 1. / static_cast<float>(k_max_);

  double log_pdf_total = 0.0;

#pragma omp parallel for schedule(static) reduction(+: log_pdf_total)
  for (int tag_index = 0; tag_index < num_tag_; tag_index++) {
    if (weights_[tag_index] == 0) continue;
    if (!std::isfinite(zvec1_[tag_index])) continue;
    if (!std::isfinite(zvec2_[tag_index])) continue;

    const double z1 = zvec1_[tag_index];
    const double z2 = zvec2_[tag_index];
    const double n1 = nvec1_[tag_index];
    const double n2 = nvec2_[tag_index];

    double pdf_tag = 0.0f;
    for (int k_index = 0; k_index < k_max_; k_index++) {
      const double tag_r2sum_c1 = (*tag_r2sum_[0])(tag_index, k_index);
      const double tag_r2sum_c2 = (*tag_r2sum_[1])(tag_index, k_index);
      const double tag_r2sum_c3 = (*tag_r2sum_[2])(tag_index, k_index);

      // Sigma  = [A1+A3  B3;  B3  C2+C3] + Sigma0 = ...
      //        = [a11    a12; a12   a22]
      const double A1 = tag_r2sum_c1 * n1 * sig2_beta[0];
      const double C2 = tag_r2sum_c2 * n2 * sig2_beta[1];
      const double A3 = tag_r2sum_c3 * n1 * sig2_beta[0];
      const double C3 = tag_r2sum_c3 * n2 * sig2_beta[1];
      const double B3 = sqrt(A3*C3) * rho_beta;

      const double a11 = A1 + A3 + a0;
      const double a22 = C2 + C3 + c0;
      const double a12 =      B3 + b0;

      const double pdf = pi_k * gaussian2_pdf_double(z1, z2, a11, a12, a22);
      pdf_tag += pdf;
    }

    log_pdf_total += -log(pdf_tag) * weights_[tag_index];
  }

  LOG << "<calc_bivariate_cost(" << ss << "), cost=" << log_pdf_total << ", elapsed time " << timer.elapsed_ms() << "ms";
  return log_pdf_total;
}

template<typename T>
std::string std_vector_to_str(const std::vector<T>& vec) {
  std::stringstream ss;
  int max_el = std::min<int>(5, vec.size() - 1);
  ss << "[";
  for (int i = 0; i < max_el; i++) {
    bool last = (i == (max_el - 1));
    ss << vec[i];
    if (last) ss << ", ...";
    else ss << ", ";
  }
  ss << "]";

  size_t nnz = 0;
  for (size_t i = 0; i < vec.size(); i++) if (vec[i] != 0) nnz++;
  ss << ", nnz=" << nnz;
  return ss.str();
}

void BgmgCalculator::log_disgnostics() {
  size_t mem_bytes = 0, mem_bytes_total = 0;
  LOG << " diag: num_snp_=" << num_snp_;
  LOG << " diag: num_tag_=" << num_tag_;
  LOG << " diag: csr_ld_snp_index_.size()=" << csr_ld_snp_index_.size();
  mem_bytes = csr_ld_tag_index_.size() * sizeof(int); mem_bytes_total += mem_bytes;
  LOG << " diag: csr_ld_tag_index_.size()=" << csr_ld_tag_index_.size() << " (mem usage = " << mem_bytes << " bytes)";
  mem_bytes = csr_ld_r2_.size() * sizeof(float); mem_bytes_total += mem_bytes;
  LOG << " diag: csr_ld_r2_.size()=" << csr_ld_r2_.size() << " (mem usage = " << mem_bytes << " bytes)";
  mem_bytes = coo_ld_.size() * (sizeof(float) + sizeof(int) + sizeof(int)); mem_bytes_total += mem_bytes;
  LOG << " diag: coo_ld_.size()=" << coo_ld_.size() << " (mem usage = " << mem_bytes << " bytes)";
  LOG << " diag: zvec1_.size()=" << zvec1_.size();
  LOG << " diag: zvec1_=" << std_vector_to_str(zvec1_);
  LOG << " diag: nvec1_.size()=" << nvec1_.size();
  LOG << " diag: nvec1_=" << std_vector_to_str(nvec1_);
  LOG << " diag: zvec2_.size()=" << zvec2_.size();
  LOG << " diag: zvec2_=" << std_vector_to_str(zvec2_);
  LOG << " diag: nvec2_.size()=" << nvec2_.size();
  LOG << " diag: nvec2_=" << std_vector_to_str(nvec2_);
  LOG << " diag: weights_.size()=" << weights_.size();
  LOG << " diag: weights_=" << std_vector_to_str(weights_);
  LOG << " diag: hvec_.size()=" << hvec_.size();
  LOG << " diag: hvec_=" << std_vector_to_str(hvec_);
  for (int i = 0; i < snp_order_.size(); i++) {
    mem_bytes = snp_order_[i]->size() * sizeof(int); mem_bytes_total += mem_bytes;
    LOG << " diag: snp_order_[" << i << "].shape=[" << snp_order_[i]->no_rows() << ", " << snp_order_[i]->no_columns() << "]" << " (mem usage = " << mem_bytes << " bytes)";
    LOG << " diag: snp_order_[" << i << "]=" << snp_order_[i]->to_str();
  }
  for (int i = 0; i < tag_r2sum_.size(); i++) {
    mem_bytes = tag_r2sum_[i]->size() * sizeof(float); mem_bytes_total += mem_bytes;
    LOG << " diag: tag_r2sum_[" << i << "].shape=[" << tag_r2sum_[i]->no_rows() << ", " << tag_r2sum_[i]->no_columns() << "]" << " (mem usage = " << mem_bytes << " bytes)";
    LOG << " diag: tag_r2sum_[" << i << "]=" << tag_r2sum_[i]->to_str();
  }
  for (int i = 0; i < last_num_causals_.size(); i++) 
    LOG << " diag: last_num_causals_[" << i << "]=" << last_num_causals_[i];
  LOG << " diag: options.k_max_=" << k_max_;
  LOG << " diag: options.max_causals_=" << max_causals_;
  LOG << " diag: options.num_components_=" << num_components_;
  LOG << " diag: options.r2_min_=" << r2_min_;
  LOG << " diag: options.use_fast_cost_calc_=" << (use_fast_cost_calc_ ? "yes" : "no");
  LOG << " diag: Estimated memory usage (total): " << mem_bytes_total << " bytes";
}

double BgmgCalculator::calc_univariate_cost_fast(float pi_vec, float sig2_zero, float sig2_beta) {
  // Use an approximation that preserves variance and kurtosis.
  // This gives a robust cost function that scales up to a very high pivec, including infinitesimal model pi==1.

  std::stringstream ss;
  ss << "calc_univariate_cost_fast(pi_vec=" << pi_vec << ", sig2_zero=" << sig2_zero << ", sig2_beta=" << sig2_beta << ")";
  LOG << ">" << ss.str();
  
  if (ld_tag_sum_r2_.empty()) calc_sum_r2_and_sum_r4();

  double log_pdf_total = 0.0;
  SimpleTimer timer(-1);

  int num_zero_tag_r2 = 0;

#pragma omp parallel for schedule(static) reduction(+: log_pdf_total)
  for (int tag_index = 0; tag_index < num_tag_; tag_index++) {
    if (weights_[tag_index] == 0) continue;
    if (!std::isfinite(zvec1_[tag_index])) continue;

    const float tag_r2 = ld_tag_sum_r2_[tag_index];
    const float tag_r4 = ld_tag_sum_r4_[tag_index];

    if (tag_r2 == 0 || tag_r4 == 0) {
      num_zero_tag_r2++; continue;
    }

    const float tag_chi = tag_r4 / tag_r2;

    const float tag_eta_factor = pi_vec * tag_r2 + (1.0f - pi_vec) * tag_chi;
    const float tag_pi1 = pi_vec * tag_r2 / tag_eta_factor;
    const float tag_pi0 = 1 - tag_pi1;
    const float tag_sig2beta = sig2_beta * tag_eta_factor;

    const float tag_z = zvec1_[tag_index];
    const float tag_n = nvec1_[tag_index];
    const float tag_pdf0 = gaussian_pdf_float(tag_z, sqrt(sig2_zero));
    const float tag_pdf1 = gaussian_pdf_float(tag_z, sqrt(sig2_zero + tag_n *tag_sig2beta));
    const float tag_pdf = tag_pi0 * tag_pdf0 + tag_pi1 * tag_pdf1;
    log_pdf_total += -log(tag_pdf) * weights_[tag_index];
  }

  if (num_zero_tag_r2 > 0)
    LOG << " warning: zero tag_r2 encountered " << num_zero_tag_r2 << " times";
  LOG << "<" << ss.str() << ", cost=" << log_pdf_total << ", elapsed time " << timer.elapsed_ms() << "ms";
  return log_pdf_total;
}

double BgmgCalculator::calc_bivariate_cost_fast(int pi_vec_len, float* pi_vec, int sig2_beta_len, float* sig2_beta, float rho_beta, int sig2_zero_len, float* sig2_zero, float rho_zero) {
  std::string ss = calc_bivariate_cost_params_to_str(pi_vec_len, pi_vec, sig2_beta_len, sig2_beta, rho_beta, sig2_zero_len, sig2_zero, rho_zero);
  LOG << ">calc_bivariate_cost_fast(" << ss << ")";

  if (ld_tag_sum_r2_.empty()) calc_sum_r2_and_sum_r4();

  double log_pdf_total = 0.0;
  SimpleTimer timer(-1);

  int num_zero_tag_r2 = 0;

  const float s0_a11 = sig2_zero[0];
  const float s0_a22 = sig2_zero[1];
  const float s0_a12 = sqrt(sig2_zero[0] * sig2_zero[1]) * rho_zero;

#pragma omp parallel for schedule(static) reduction(+: log_pdf_total)
  for (int tag_index = 0; tag_index < num_tag_; tag_index++) {
    if (weights_[tag_index] == 0) continue;
    if (!std::isfinite(zvec1_[tag_index])) continue;
    if (!std::isfinite(zvec2_[tag_index])) continue;

    const float z1 = zvec1_[tag_index];
    const float n1 = nvec1_[tag_index];
    const float z2 = zvec2_[tag_index];
    const float n2 = nvec2_[tag_index];

    const float tag_r2 = ld_tag_sum_r2_[tag_index];
    const float tag_r4 = ld_tag_sum_r4_[tag_index];

    if (tag_r2 == 0 || tag_r4 == 0) {
      num_zero_tag_r2++; continue;
    }

    const float tag_chi = tag_r4 / tag_r2;

    const float tag_eta_factor[3] = {
      pi_vec[0] * tag_r2 + (1.0f - pi_vec[0]) * tag_chi,
      pi_vec[1] * tag_r2 + (1.0f - pi_vec[1]) * tag_chi,
      pi_vec[2] * tag_r2 + (1.0f - pi_vec[2]) * tag_chi
    };

    const float tag_pi1[3] = {
      pi_vec[0] * tag_r2 / tag_eta_factor[0],
      pi_vec[1] * tag_r2 / tag_eta_factor[1],
      pi_vec[2] * tag_r2 / tag_eta_factor[2]
    };

    const float tag_pi0[3] = {
      1.0f - tag_pi1[0],
      1.0f - tag_pi1[1],
      1.0f - tag_pi1[2]
    };

    const float a11[3] = { tag_eta_factor[0] * n1 * sig2_beta[0], 0,                                     tag_eta_factor[2] * n1 * sig2_beta[0] };
    const float a22[3] = { 0,                                     tag_eta_factor[1] * n2 * sig2_beta[1], tag_eta_factor[2] * n2 * sig2_beta[1] };
    const float a12[3] = { 0,                                     0,                                     rho_beta * sqrt(a11[2] * a22[2]) };

    const float f0[8] = { 0,0,0,0,1,1,1,1 };
    const float f1[8] = { 0,0,1,1,0,0,1,1 };
    const float f2[8] = { 0,1,0,1,0,1,0,1 };

    float tag_pdf = 0.0f;
    for (int i = 0; i < 8; i++) {
      const float pi1 = (f0[i] ? tag_pi1[0] : tag_pi0[0]);
      const float pi2 = (f1[i] ? tag_pi1[1] : tag_pi0[1]);
      const float pi3 = (f2[i] ? tag_pi1[2] : tag_pi0[2]);
      const float a11i = s0_a11 + f0[i] * a11[0] + f1[i] * a11[1] + f2[i] * a11[2];
      const float a22i = s0_a22 + f0[i] * a22[0] + f1[i] * a22[1] + f2[i] * a22[2];
      const float a12i = s0_a12 + f0[i] * a12[0] + f1[i] * a12[1] + f2[i] * a12[2];
      tag_pdf += (pi1*pi2*pi3) * gaussian2_pdf_double(z1, z2, a11i, a12i, a22i);
    }

    log_pdf_total += static_cast<double>(-log(tag_pdf) * weights_[tag_index]);
  }

  if (num_zero_tag_r2 > 0)
    LOG << " warning: zero tag_r2 encountered " << num_zero_tag_r2 << " times";

  LOG << "<calc_bivariate_cost_fast(" << ss << "), cost=" << log_pdf_total << ", elapsed time " << timer.elapsed_ms() << "ms";
  return log_pdf_total;
}

void BgmgCalculator::clear_state() {
  LOG << " clear_state";

  // clear all info about LD structure
  csr_ld_snp_index_.clear();
  csr_ld_tag_index_.clear();
  csr_ld_r2_.clear();
  coo_ld_.clear();
  hvec_.clear();
  ld_tag_sum_r2_.clear();
  ld_tag_sum_r4_.clear();

  // clear ordering of SNPs
  snp_order_.clear();
  tag_r2sum_.clear();
  last_num_causals_.clear();
  snp_can_be_causal_.clear();
}

void BgmgCalculator::clear_tag_r2sum(int component_id) {
  if (component_id < 0 || component_id >= num_components_) BOOST_THROW_EXCEPTION(::std::runtime_error("find_tag_r2sum: component_id must be between 0 and num_components_"));
  if (last_num_causals_.empty()) return;
  LOG << " clear_tag_r2sum(component_id=" << component_id << ")";
  last_num_causals_[component_id] = 0;
  tag_r2sum_[component_id]->InitializeZeros();
}

void BgmgCalculator::calc_sum_r2_and_sum_r4() {
  LOG << ">calc_sum_r2_and_sum_r4()";
  SimpleTimer timer(-1);
  SimpleTimer log_timer(10000);

  ld_tag_sum_r2_.clear(); ld_tag_sum_r2_.resize(num_tag_, 0);
  ld_tag_sum_r4_.clear(); ld_tag_sum_r4_.resize(num_tag_, 0);
  for (int causal_index = 0; causal_index < num_snp_; causal_index++) {
    if (log_timer.fire()) LOG << " find_snp_order still working, snp_index=" << causal_index;

    const int r2_index_from = csr_ld_snp_index_[causal_index];
    const int r2_index_to = csr_ld_snp_index_[causal_index+1];
    for (int r2_index = r2_index_from; r2_index < r2_index_to; r2_index++) {
      const int tag_index = csr_ld_tag_index_[r2_index];
      const float r2 = csr_ld_r2_[r2_index];
      ld_tag_sum_r2_[tag_index] += r2;
      ld_tag_sum_r4_[tag_index] += (r2 * r2);
    }
  }

  LOG << "<calc_sum_r2_and_sum_r4(), elapsed time " << timer.elapsed_ms() << "ms";
}

int64_t BgmgCalculator::set_weights_randprune(int n, float r2_threshold) {
  LOG << ">set_weights_randprune(n=" << n << ", r2=" << r2_threshold << ")";
  if (r2_threshold < r2_min_) BOOST_THROW_EXCEPTION(::std::runtime_error("set_weights_randprune: r2 < r2_min_"));
  if (n <= 0) BOOST_THROW_EXCEPTION(::std::runtime_error("set_weights_randprune: n <= 0"));
  if (!hvec_.empty()) BOOST_THROW_EXCEPTION(::std::runtime_error("set_weights_randprune must be called before set_hvec"));
  SimpleTimer timer(-1);

  std::valarray<int> passed_random_pruning(0, num_tag_);  // count how many times an index  has passed random pruning

#pragma omp parallel
  {
    std::valarray<int> passed_random_pruning_local(0, num_tag_);  // count how many times an index  has passed random pruning

#pragma omp for schedule(static)
    for (int prune_i = 0; prune_i < n; prune_i++) {
      std::mt19937_64 random_engine;
      random_engine.seed(prune_i);

      std::vector<int> candidate_tag_indices(num_tag_, 0);
      std::vector<char> processed_tag_indices(num_tag_, 0);
      for (int i = 0; i < num_tag_; i++) candidate_tag_indices[i] = i;
      std::set<int> non_processed_tag_indices(candidate_tag_indices.begin(), candidate_tag_indices.end());

      while (candidate_tag_indices.size() > 0) {
        // Here is the logic:
        // 1. select a random element X from the candidate_tag_indices
        // 2. if X is present in processed_tag_indices (collision case):
        //    - re-generate candidate_tag_indices from the set of non_processed_tag_indices
        //    - continue while loop.
        // 3. add X to passed_random_pruning
        // 4. query LD matrix for everything in LD with X (we asume that X will be part of that list). Then, for each Y in LD with X:
        //    - add Y to processed_tag_indices
        //    - remove Y from non_processed_tag_indices

        const int random_candidate_index = std::uniform_int_distribution<int>(0, candidate_tag_indices.size() - 1)(random_engine);
        const int random_tag_index = candidate_tag_indices[random_candidate_index];
        if (processed_tag_indices[random_tag_index]) {
          candidate_tag_indices.assign(non_processed_tag_indices.begin(), non_processed_tag_indices.end());
          continue;
        }

        passed_random_pruning_local[random_tag_index] += 1;
        int causal_index = tag_to_snp_[random_tag_index];
        const int r2_index_from = csr_ld_snp_index_[causal_index];
        const int r2_index_to = csr_ld_snp_index_[causal_index + 1];
        for (int r2_index = r2_index_from; r2_index < r2_index_to; r2_index++) {
          const int tag_index = csr_ld_tag_index_[r2_index];
          const float r2_value = csr_ld_r2_[r2_index];
          if (r2_value < r2_threshold) continue;
          if (processed_tag_indices[tag_index]) continue;
          processed_tag_indices[tag_index] = 1;         //  mark as processed, and
          non_processed_tag_indices.erase(tag_index);   // remove from the set
        }
      }
    }

#pragma omp critical
    passed_random_pruning += passed_random_pruning_local;
  }

  weights_.clear(); weights_.resize(num_tag_, 0.0f);
  for (int i = 0; i < num_tag_; i++)
    weights_[i] = static_cast<float>(passed_random_pruning[i]) / static_cast<float>(n);

  LOG << ">set_weights_randprune(n=" << n << ", r2=" << r2_threshold << "), elapsed time " << timer.elapsed_ms() << "ms";
  return 0;
}

int64_t BgmgCalculator::retrieve_weights(int length, float* buffer) {
  if (length != num_tag_) BOOST_THROW_EXCEPTION(::std::runtime_error("wrong buffer size"));
  if (weights_.size() != num_tag_) BOOST_THROW_EXCEPTION(::std::runtime_error("weights_.size() != num_tag_"));
  LOG << " retrieve_weights()";
  for (int i = 0; i < num_tag_; i++) buffer[i] = weights_[i];
  return 0;
}