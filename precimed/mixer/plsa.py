#!/usr/bin/env python
'''
(c) 2018-2019 Oleksandr Frei, Alexey A. Shadrin
MiXeR software: Univariate and Bivariate Causal Mixture for GWAS
'''

import sys
import os
import argparse
import numpy as np
from numpy import ma
import pandas as pd
import scipy.optimize
import logging
import json
import scipy.stats
from scipy.interpolate import interp1d
from datetime import datetime

from .libbgmg import LibBgmg
from .utils import AnnotUnivariateParams
from .utils import AnnotUnivariateParametrization
from .utils import _log_exp_converter
from .utils import _logit_logistic_converter
from .utils import _arctanh_tanh_converter
from .utils import calc_qq_data
from .utils import calc_qq_model
from .utils import calc_qq_plot
from .utils import NumpyEncoder

__version__ = '1.0.0'
MASTHEAD = "***********************************************************************\n"
MASTHEAD += "* mixer_plsa.py: Annotation-Informed Causal Mixture\n"
MASTHEAD += "*                for MAF- and LD- dependent genetic architectures\n"
MASTHEAD += "* Version {V}\n".format(V=__version__)
MASTHEAD += "* (c) 2018-2019 Oleksandr Frei, Alexey A. Shadrin\n"
MASTHEAD += "* Norwegian Centre for Mental Disorders Research / University of Oslo\n"
MASTHEAD += "* GNU General Public License v3\n"
MASTHEAD += "***********************************************************************\n"

_cost_calculator_sampling = 0
_cost_calculator_gaussian = 1
_cost_calculator_convolve = 2

_cost_calculator = {
    'gaussian': _cost_calculator_gaussian, 
    'sampling': _cost_calculator_sampling,
    'convolve': _cost_calculator_convolve,
}

def convert_args_to_libbgmg_options(args, num_snp):
    libbgmg_options = {
        'r2min': args.r2min, 'kmax': args.kmax, 
        'threads': args.threads, 'seed': args.seed,
        'cubature_rel_error': args.cubature_rel_error,
        'cubature_max_evals':args.cubature_max_evals
    }
    return [(k, v) for k, v in libbgmg_options.items() if v is not None ]

def fix_and_validate_args(args):
    arg_dict = vars(args)
    chr2use_arg = arg_dict["chr2use"]
    chr2use = []
    for a in chr2use_arg.split(","):
        if "-" in a:
            start, end = [int(x) for x in a.split("-")]
            chr2use += [str(x) for x in range(start, end+1)]
        else:
            chr2use.append(a.strip())
    if np.any([not x.isdigit() for x in chr2use]): raise ValueError('Chromosome labels must be integer')
    arg_dict["chr2use"] = [int(x) for x in chr2use]

# https://stackoverflow.com/questions/27433316/how-to-get-argparse-to-read-arguments-from-a-file-with-an-option-rather-than-pre
class LoadFromFile (argparse.Action):
    def __call__ (self, parser, namespace, values, option_string=None):
        with values as f:
            contents = f.read()

        data = parser.parse_args(contents.split(), namespace=namespace)
        for k, v in vars(data).items():
            if v and k != option_string.lstrip('-'):
                setattr(namespace, k, v)

def parser_fit_add_arguments(args, func, parser):
    parser.add_argument("--bim-file", type=str, default=None, help="Plink bim file. "
        "Defines the reference set of SNPs used for the analysis. "
        "Marker names must not have duplicated entries. "
        "May contain simbol '@', which will be replaced with the actual chromosome label. ")
    parser.add_argument("--frq-file", type=str, default=None, help="Plink frq file (alleles frequencies). "
        "May contain simbol '@', similarly to --bim-file argument. ")
    parser.add_argument("--plink-ld-bin", type=str, default=None, help="File with linkage disequilibrium information, "
        "converted from plink format as described in the README.md file. "
        "May contain simbol '@', similarly to --bim-file argument. ")
    parser.add_argument("--plink-ld-bin0", type=str, default=None, help="File with linkage disequilibrium information in an old format (deprecated)")
    parser.add_argument("--chr2use", type=str, default="1-22", help="Chromosome ids to use "
         "(e.g. 1,2,3 or 1-4,12,16-20). Chromosome must be labeled by integer, i.e. X and Y are not acceptable. ")
    parser.add_argument("--trait1-file", type=str, default=None, help="GWAS summary statistics for the first trait. ")
    parser.add_argument("--annot-file", type=str, default=None, help="Path to binary annotations in LD score regression format, i.e. <path>/baseline.@.annot.gz")

    parser.add_argument('--extract', type=str, default="", help="File with variants to include in the fit procedure")
    parser.add_argument('--exclude', type=str, default="", help="File with variants to exclude from the fit procedure")

    parser.add_argument('--randprune-n', type=int, default=64, help="Number of random pruning iterations")
    parser.add_argument('--randprune-r2', type=float, default=0.1, help="Threshold for random pruning")
    parser.add_argument('--kmax', type=int, default=100, help="Number of sampling iterations")
    parser.add_argument('--seed', type=int, default=123, help="Random seed")

    parser.add_argument('--r2min', type=float, default=0.05, help="r2 values below this threshold will contribute via infinitesimal model")
    parser.add_argument('--threads', type=int, default=None, help="specify how many threads to use (concurrency). None will default to the total number of CPU cores. ")
    parser.add_argument('--tol-x', type=float, default=1e-2, help="tolerance for the stop criteria in fminsearch optimization. ")
    parser.add_argument('--tol-func', type=float, default=1e-2, help="tolerance for the stop criteria in fminsearch optimization. ")
    parser.add_argument('--cubature-rel-error', type=float, default=1e-5, help="relative error for cubature stop criteria (applies to 'convolve' cost calculator). ")
    parser.add_argument('--cubature-max-evals', type=float, default=1000, help="max evaluations for cubature stop criteria (applies to 'convolve' cost calculator). ")
    parser.add_argument('--qq-plots', default=False, action="store_true", help="generate qq plot curves")    
    parser.add_argument('--cost', default=False, action="store_true", help="save full cost function")
    parser.add_argument('--models', default=[1,2,3,4,5,6,7,8,9,50,51,52], type=int, nargs='+', choices=[1,2,3,4,5,6,7,8,9,50,51,52])

    parser.add_argument('--downsample-factor', default=50, type=int, help="Applies to --power-curve. "
        "'--downsample-factor N' imply that only 1 out of N available z-score values will be used in calculations.")

    parser.set_defaults(func=func)

def parse_args(args):
    parser = argparse.ArgumentParser(description="PLSA-MiXeR: Annotation-Informed Causal Mixture for MAF- and LD- dependent genetic architectures.")

    parent_parser = argparse.ArgumentParser(add_help=False)
    parent_parser.add_argument('--argsfile', type=open, action=LoadFromFile, default=None, help="file with additional command-line arguments")
    parent_parser.add_argument("--out", type=str, default="mixer", help="prefix for the output files")
    parent_parser.add_argument("--lib", type=str, default="libbgmg.so", help="path to libbgmg.so plugin")
    parent_parser.add_argument("--log", type=str, default=None, help="file to output log, defaults to <out>.log")
    
    subparsers = parser.add_subparsers()

    parser_fit_add_arguments(args=args, func=execute_fit_parser, parser=subparsers.add_parser("fit", parents=[parent_parser], help='fit MiXeR model'))

    return parser.parse_args(args)

def log_header(args, subparser_name, lib):
    defaults = vars(parse_args([subparser_name]))
    opts = vars(args)
    non_defaults = [x for x in opts.keys() if opts[x] != defaults[x]]
    header = MASTHEAD
    header += "Call: \n"
    header += './mixer_plsa.py {} \\\n'.format(subparser_name)
    options = ['\t--'+x.replace('_','-')+' '+str(opts[x]).replace('\t', '\\t')+' \\' for x in non_defaults]
    header += '\n'.join(options).replace('True','').replace('False','')
    header = header[0:-1]+'\n'
    lib.log_message(header)

# helper function to debug non-json searizable types...
def print_types(results, libbgmg):
    if isinstance(results, dict):
        for k, v in results.items():
            libbgmg.log_message('{}: {}'.format(k, type(v)))
            print_types(v, libbgmg)

def enhance_optimize_result(r, cost_n, cost_df=None, cost_fast=None):
    # optimize_result - an instance of scipy.optimize.OptimizeResult
    # const_n - number of genetic variants effectively contributing to the cost (sum of weights)
    r['cost_n'] = cost_n
    r['cost_df'] = len(r.x) if (cost_df is None) else cost_df
    r['cost'] = r.fun
    r['BIC'] = np.log(r['cost_n']) * r['cost_df'] + 2 * r['cost']
    r['AIC'] =                   2 * r['cost_df'] + 2 * r['cost']
    if cost_fast is not None: r['cost_fast'] = cost_fast

def perform_fit(bounds_left, bounds_right, constraint, args, annomat, annonames, lib, trait_index):
    mafvec = lib.mafvec
    tldvec = lib.ld_tag_r2_sum  # this is available for tag indices only, hense we enabled use_complete_tag_indices
    optimize_result_sequence = []
    results = {}

    # for QQ plots - but here it makes no difference as we use complete tag indices 
    mafvec_tag = mafvec[lib.defvec]
    tldvec_tag = tldvec

    if (bounds_left is not None) and (bounds_right is not None):
        parametrization = AnnotUnivariateParametrization(lib=lib, trait=trait_index, constraint=constraint)
        
        lib.set_option('cost_calculator', _cost_calculator_gaussian)
    
        # Step1, diffevo-fast
        bounds4opt = [(l, r) for l, r in zip(parametrization.params_to_vec(bounds_left), parametrization.params_to_vec(bounds_right))]
        optimize_result = scipy.optimize.differential_evolution(lambda x: parametrization.calc_cost(x), bounds4opt,
            tol=0.01, mutation=(0.5, 1), recombination=0.7, atol=0, updating='immediate', polish=False, workers=1)  #, **global_opt_options)
        params = parametrization.vec_to_params(optimize_result.x)
        enhance_optimize_result(optimize_result, cost_n=np.sum(lib.weights), cost_fast=params.cost(lib, trait_index))
        optimize_result['params']=params.as_dict()   # params after optimization
        optimize_result_sequence.append(('diffevo-fast', optimize_result))

        # Step 2. neldermead-fast
        optimize_result = scipy.optimize.minimize(lambda x: parametrization.calc_cost(x), parametrization.params_to_vec(params),
            method='Nelder-Mead', options={'maxiter':480, 'fatol':1e-7, 'xatol':1e-4, 'adaptive':True})
        params = parametrization.vec_to_params(optimize_result.x)
        enhance_optimize_result(optimize_result, cost_n=np.sum(lib.weights), cost_fast=params.cost(lib, trait_index))
        optimize_result['params']=params.as_dict()   # params after optimization
        optimize_result_sequence.append(('neldermead-fast', optimize_result))

        # Step 3. neldermead (for non-infinitesimal model)
        if (len(params._pi) > 1) or (params._pi[0] != 1):
            lib.set_option('cost_calculator', _cost_calculator_convolve)
            optimize_result = scipy.optimize.minimize(lambda x: parametrization.calc_cost(x), parametrization.params_to_vec(params),
                method='Nelder-Mead', options={'maxiter':480, 'fatol':1e-7, 'xatol':1e-4, 'adaptive':True})
            params = parametrization.vec_to_params(optimize_result.x)
            enhance_optimize_result(optimize_result, cost_n=np.sum(lib.weights), cost_fast=params.cost(lib, trait_index))
            optimize_result['params']=params.as_dict()   # params after optimization
            optimize_result_sequence.append(('neldermead', optimize_result))
        else:
            lib.log_message('Gaussian cost function is correct for infinitesimal models, skip fit with full cost function')
    else:
        params=constraint

    results['params'] = params.as_dict()
    results['optimize'] = optimize_result_sequence
    results['annot_enrich'] = params.find_annot_enrich(annomat).flatten()
    results['annot_h2'] = params.find_annot_h2(annomat).flatten()

    if args.cost:
        lib.set_option('cost_calculator', _cost_calculator_convolve)
        results['convolve_tag_pdf'] = params.tag_pdf(lib, trait_index)[lib.weights>0]
        results['convolve_tag_pdf_err'] = params.tag_pdf_err(lib, trait_index)[lib.weights>0]

        lib.set_option('cost_calculator', _cost_calculator_sampling)
        lib.set_option('kmax', 20000)  # hard-code kmax for cost function calculation
        results['sampling_tag_pdf'] = params.tag_pdf(lib, trait_index)[lib.weights>0]
        lib.set_option('kmax', args.kmax)

        lib.set_option('cost_calculator', _cost_calculator_gaussian)
        results['gaussian_tag_pdf'] = params.tag_pdf(lib, trait_index)[lib.weights>0]

    if args.qq_plots:
        defvec = np.isfinite(lib.get_zvec(trait_index)) & (lib.weights > 0)
        results['qqplot'] = calc_qq_plot(lib, params, trait_index, args.downsample_factor, defvec,
            title='maf \\in [{:.3g},{:.3g}); L \\in [{:.3g},{:.3g})'.format(-np.inf,np.inf,-np.inf,np.inf))

        maf_bins = np.concatenate(([-np.inf], np.quantile(mafvec_tag, [1/3, 2/3]), [np.inf]))
        tld_bins = np.concatenate(([-np.inf], np.quantile(tldvec_tag, [1/3, 2/3]), [np.inf]))
        results['qqplot_bins'] = []
        for i in range(0, 3):
            for j in range(0, 3):
                mask = (defvec & (mafvec_tag>=maf_bins[i]) & (mafvec_tag<maf_bins[i+1]) & (tldvec_tag >= tld_bins[j]) & (tldvec_tag < tld_bins[j+1]))
                results['qqplot_bins'].append(calc_qq_plot(lib, params, trait_index, args.downsample_factor, mask,
                    title='maf \\in [{:.3g},{:.3g}); L \\in [{:.3g},{:.3g})'.format(maf_bins[i], maf_bins[i+1], tld_bins[j], tld_bins[j+1])))

    return results, params

def execute_fit_parser(args):
    libbgmg = LibBgmg(args.lib)

    # resolve dependencies between models
    if ((1 not in args.models) and bool(set(args.models) & set([5,7]))): args.models.append(1)
    if ((2 not in args.models) and bool(set(args.models) & set([6,8,9]))): args.models.append(2)

    fix_and_validate_args(args)
    
    libbgmg.set_option('use_complete_tag_indices', 1)
    libbgmg.set_option('cost_calculator', _cost_calculator_gaussian)
    libbgmg.init(args.bim_file, args.frq_file, args.chr2use, args.trait1_file, "", args.exclude, args.extract)

    # Load annotations
    libbgmg.log_message('Loading annotations from {}...'.format(args.annot_file))
    df = pd.concat([pd.read_csv(args.annot_file.replace('@', str(chr_label)), sep='\t') for chr_label in args.chr2use])
    for col in ['CHR', 'BP', 'SNP', 'CM']:
        if col in df.columns:
            del df[col]

    annomat = df.values.astype(np.float32)
    annonames = df.columns.values
    del df
    libbgmg.log_message('Done, {} annotations available'.format(len(annonames)))
    

    for opt, val in convert_args_to_libbgmg_options(args, libbgmg.num_snp):
        libbgmg.set_option(opt, val)

    if args.plink_ld_bin0 is not None:
        libbgmg.set_option('ld_format_version', 0)
        args.plink_ld_bin = args.plink_ld_bin0
        args.plink_ld_bin0 = None

    for chr_label in args.chr2use: 
        libbgmg.set_ld_r2_coo_from_file(chr_label, args.plink_ld_bin.replace('@', str(chr_label)))
        libbgmg.set_ld_r2_csr(chr_label)

    libbgmg.set_weights_randprune(args.randprune_n, args.randprune_r2)
    libbgmg.set_option('diag', 0)

    totalhet = float(2.0 * np.dot(libbgmg.mafvec, 1.0 - libbgmg.mafvec))
    mafvec = libbgmg.mafvec[libbgmg.defvec]
    tldvec = libbgmg.ld_tag_r2_sum
    trait_index = 1

    results = {}
    results['options'] = vars(args).copy()
    results['options']['totalhet'] = totalhet
    results['options']['num_snp'] = float(libbgmg.num_snp)
    results['options']['num_tag'] = float(libbgmg.num_tag)
    results['options']['sum_weights'] = float(np.sum(libbgmg.weights))
    results['options']['trait1_nval'] = float(np.nanmedian(libbgmg.get_nvec(trait=1)))
    results['options']['annonames'] = annonames
    results['options']['time_started'] = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    results['analysis'] = 'mixer-plsa'
    results['weights'] = libbgmg.weights[libbgmg.weights>0]
    results['zvec1'] = libbgmg.zvec1[libbgmg.weights>0]

    # overview of the models
    # params1 - basic infinitesimal model
    # params2 - infinitesimal model with flexible s and l parameters
    # params3 - basic causal mixture model
    # params4 - causal mixture model with flexible s and l parameters
    # params5 - infinitesimal model with annotations 
    # params6 - infinitesimal model with annotations and flexible s and l parameters
    # params7 - causal mixture model with annotations
    # params8 - causal mixture model with annotations and flexible s and l parameters
    # params9 - causal mixture model with annotations and flexible s and l parameters - s and l re-fitted in the context of a causal mixture

    # params50 - basic infinitesimal model with s=-1 (LDSC assumptions)
    # params51 - a model with infinitesimal and causal mixture 
    # params52 - a model with two causal components (M3)

    if 52 in args.models:
        # Fit params52 - a model with two causal components (M3)
        result, params52 = perform_fit(AnnotUnivariateParams(pi=[5e-5, 5e-5], sig2_beta=[5e-8, 5e-8], sig2_zeroA=0.9),
                                    AnnotUnivariateParams(pi=[5e-1, 5e-1], sig2_beta=[5e-2, 5e-2], sig2_zeroA=2.5),
                                    AnnotUnivariateParams(pi=[None, None], sig2_beta=[None, None], sig2_annot=[1], s=0, l=0,
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params52'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 51 in args.models:
        #  Fit params51 - a model with infinitesimal and causal mixture 
        result, params51 = perform_fit(AnnotUnivariateParams(pi=[None, 5e-5], sig2_beta=[5e-8, 5e-8], sig2_zeroA=0.9),
                                    AnnotUnivariateParams(pi=[None, 5e-1], sig2_beta=[5e-2, 5e-2], sig2_zeroA=2.5),
                                    AnnotUnivariateParams(pi=[1, None], sig2_beta=[None, None], sig2_annot=[1], s=0, l=0,
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params51'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 50 in args.models:
        # Fit params50 - basic infinitesimal model with LDSC assumtions
        result, params50 = perform_fit(AnnotUnivariateParams(sig2_beta=5e-8, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(pi=1, sig2_annot=[1], s=-1, l=0,
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params50'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 1 in args.models:
        # Fit params1 - basic infinitesimal model
        result, params1 = perform_fit(AnnotUnivariateParams(sig2_beta=5e-8, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(pi=1, sig2_annot=[1], s=0, l=0,
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params1'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 2 in args.models:
        # Fit params2 - infinitesimal model with flexible s and l parameters
        result, params2 = perform_fit(AnnotUnivariateParams(s=-1.0, l=-1.0, sig2_beta=5e-8, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(s=0.25, l=0.25, sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(pi=1, sig2_annot=[1], 
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params2'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 3 in args.models:
        # Fit params3 - basic causal mixture model
        result, params3 = perform_fit(AnnotUnivariateParams(pi=5e-5, sig2_beta=5e-6, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(pi=5e-1, sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(sig2_annot=[1], s=0, l=0,
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params3'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 4 in args.models:
        # Fit params4 - causal mixture model with flexible s and l parameters
        result, params4 = perform_fit(AnnotUnivariateParams(s=-1.0, l=-1.0, pi=5e-5, sig2_beta=5e-6, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(s=0.25, l=0.25, pi=5e-1, sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(sig2_annot=[1],
                                                            annomat=annomat[:, 0].reshape(-1, 1), annonames=[annonames[0]],
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params4'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 5 in args.models:
        # Fit params5 - infinitesimal model with annotations 
        params_tmp = AnnotUnivariateParams(pi=1.0, s=0, l=0, sig2_beta=params1._sig2_beta, sig2_zeroA=0,
                                        annomat=annomat, annonames=annonames, mafvec=mafvec, tldvec=tldvec)
        params_tmp.fit_sig2_annot(libbgmg, trait_index); params_tmp.drop_zero_annot()
        result, params5 = perform_fit(None, None, params_tmp,
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params5'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 6 in args.models:
        # Fit params6 - infinitesimal model with annotations and flexible s and l parameters
        params_tmp = AnnotUnivariateParams(pi=1.0, s=params2._s, l=params2._l, sig2_beta=params2._sig2_beta, sig2_zeroA=0,
                                        annomat=annomat, annonames=annonames, mafvec=mafvec, tldvec=tldvec)
        params_tmp.fit_sig2_annot(libbgmg, trait_index); params_tmp.drop_zero_annot()
        result, params6 = perform_fit(None, None, params_tmp,
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params6'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 7 in args.models:
        # Fit params7 - causal mixture model with annotations
        params_tmp = AnnotUnivariateParams(pi=1.0, s=0, l=0, sig2_beta=params1._sig2_beta, sig2_zeroA=0,
                                        annomat=annomat, annonames=annonames, mafvec=mafvec, tldvec=tldvec)
        params_tmp.fit_sig2_annot(libbgmg, trait_index); params_tmp.drop_zero_annot()
        result, params7 = perform_fit(AnnotUnivariateParams(pi=5e-5, sig2_beta=5e-6, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(pi=5e-1, sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(s=0, l=0, sig2_annot=params_tmp._sig2_annot,
                                                            annomat=params_tmp._annomat, annonames=params_tmp._annonames,
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params7'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 8 in args.models:
        # Fit params8 - causal mixture model with annotations and flexible s and l parameters
        params_tmp = AnnotUnivariateParams(pi=1.0, s=params2._s, l=params2._l, sig2_beta=params2._sig2_beta, sig2_zeroA=0,
                                        annomat=annomat, annonames=annonames, mafvec=mafvec, tldvec=tldvec)
        params_tmp.fit_sig2_annot(libbgmg, trait_index); params_tmp.drop_zero_annot()
        result, params8 = perform_fit(AnnotUnivariateParams(pi=5e-5, sig2_beta=5e-6, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(pi=5e-1, sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(s=params_tmp._s, l=params_tmp._l, sig2_annot=params_tmp._sig2_annot,
                                                            annomat=params_tmp._annomat, annonames=params_tmp._annonames,
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params8'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    if 9 in args.models:
        # Fit params9 - causal mixture model with annotations and flexible s and l parameters - s and l re-fitted in the context of a causal mixture
        params_tmp = AnnotUnivariateParams(pi=1.0, s=params2._s, l=params2._l, sig2_beta=params2._sig2_beta, sig2_zeroA=0,
                                        annomat=annomat, annonames=annonames, mafvec=mafvec, tldvec=tldvec)
        params_tmp.fit_sig2_annot(libbgmg, trait_index); params_tmp.drop_zero_annot()
        result, params9 = perform_fit(AnnotUnivariateParams(s=-1.0, l=-1.0, pi=5e-5, sig2_beta=5e-6, sig2_zeroA=0.9),
                                    AnnotUnivariateParams(s=0.25, l=0.25, pi=5e-1, sig2_beta=5e-2, sig2_zeroA=2.5),
                                    AnnotUnivariateParams(sig2_annot=params_tmp._sig2_annot,
                                                            annomat=params_tmp._annomat, annonames=params_tmp._annonames,
                                                            mafvec=mafvec, tldvec=tldvec),
                                    args, annomat, annonames, libbgmg, trait_index)
        results['params9'] = result
        with open(args.out + '.tmp.json', 'w') as outfile:
            json.dump(results, outfile, cls=NumpyEncoder)

    results['options']['time_finished'] = datetime.now().strftime('%Y-%m-%d %H:%M:%S')

    with open(args.out + '.json', 'w') as outfile:
        json.dump(results, outfile, cls=NumpyEncoder)

    libbgmg.set_option('diag', 0)
    libbgmg.log_message('Done')

