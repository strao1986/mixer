if libisloaded('bgmg'), unloadlibrary('bgmg'); end;
if ~libisloaded('bgmg'), loadlibrary('H:\GitHub\BGMG\src\build_win\bin\RelWithDebInfo\bgmg.dll', 'H:\GitHub\BGMG\src\bgmg_matlab.h'); end;

%loadlibrary('H:\GitHub\BGMG\src\build_win\bin\Debug\bgmg.dll', 'H:\GitHub\BGMG\src\bgmg_matlab.h');
%libfunctions('bgmg')
%unloadlibrary('bgmg');

if ~exist('ref', 'var')
ref = load('H:\Dropbox\shared\BGMG\HAPGEN_EUR_100K_11015883_reference_bfile_merged_ldmat_p01_SNPwind50k_per_allele_4bins.mat');
end

hits = sum(ref.pruneidxmat, 2); weights = hits / size(ref.pruneidxmat, 2);

def2= load('H:\Dropbox\shared\BGMG\defvec_hapmap3.mat');

defvec = ref.defvec & (weights > 0) ; % & (ref.chrnumvec == 1);
defvec = defvec & def2.defvec;
tag_indices = find(defvec);
weights(defvec) = 1;

m2c = @(x)(x-1); % convert matlab to c indices
check = @()fprintf('RESULT: %s; STATUS: %s\n', calllib('bgmg', 'bgmg_get_last_error'), calllib('bgmg', 'bgmg_status', 0));
context=0;kmax = 5000; num_components=3;
calllib('bgmg', 'bgmg_set_tag_indices', 0, length(ref.defvec), length(tag_indices), m2c(tag_indices));  check();
calllib('bgmg', 'bgmg_set_option', 0,  'r2min', 0.01); check();
calllib('bgmg', 'bgmg_set_option', 0,  'kmax', kmax); check();
calllib('bgmg', 'bgmg_set_option', 0,  'max_causals', 200000); check();  
calllib('bgmg', 'bgmg_set_option', 0,  'num_components', num_components); check();

fprintf('Loading LD structure...\n');
for c= 22:-1:1
    fprintf('chr %i...\n', c);
    c1 = load(['H:\work\hapgen_ldmat2_plink\', sprintf('bfile_merged_10K_ldmat_p01_SNPwind50k_chr%i.ld.mat', c)]);
    c1.index_A = c1.index_A + 1; 
    c1.index_B = c1.index_B + 1; % 0-based, comming from python
    calllib('bgmg', 'bgmg_set_ld_r2_coo', 0, length(c1.r2), m2c(c1.index_A), m2c(c1.index_B), c1.r2);  check();
%    clear('c1');
end

tic
calllib('bgmg', 'bgmg_set_ld_r2_csr', 0);  check();
hvec = ref.mafvec .* (1-ref.mafvec) * 2;
calllib('bgmg', 'bgmg_set_hvec', 0, length(hvec), hvec);  check();
toc





trait=1; 

%calllib('bgmg', 'bgmg_set_zvec', 0, trait, sum(defvec), zvec(defvec));  check();
%calllib('bgmg', 'bgmg_set_nvec', 0, trait, sum(defvec), nvec(defvec));  check();
calllib('bgmg', 'bgmg_set_weights', 0, sum(defvec), weights(defvec));  check();

% weights(defvec)=1;calllib('bgmg', 'bgmg_set_weights', 0, sum(defvec), weights(defvec));  check();



%dat = load('H:\NORSTORE\oleksanf\11015833\simu_ugmg_120_traits\simu_h2=0.1_pi1u=0.0001_rep=3.trait1.mat');zvec = dat.zvec;
%nvec = ones(size(zvec)) * 100000;

%def1= load('H:\Dropbox\shared\BGMG\defvec_HAPGEN_EUR_100K.mat'); def1=def1.defvec; def1 = def1(1:num_snp);
%def2= load('H:\Dropbox\shared\BGMG\defvec_hapmap3.mat'); def2=def2.defvec; def2 = def2(1:num_snp);
%defvec = def1 & def2 & isfinite(zvec) & (weights>=1); clear('def1', 'def2');
%hvec = ref.hvec; %2 * ref.mafvec(1:num_snp) .* ref.mafvec(1:num_snp);

% idea1 - [DONE] smooth num_causals (float-point) to avoid jumps in cost functions - implemented
% idea2 - [DONE] ignore low z scores , e.i. fit tail only - didn't help
% idea3 - TBD: boost kmax (but keep little info about LD structure by permuting snp_order
% idea4 - make snp_order such that each SNP in the template became causal at least once.

if 0
figure(2)
num_snp = length(zvec); 
pBuffer = libpointer('singlePtr', zeros(kmax*sum(defvec), 1, 'single'));
calllib('bgmg', 'bgmg_retrieve_tag_r2_sum', 0, 0, round(dat.causal_pi * num_snp), kmax*sum(defvec), pBuffer);  check(); 
tag_r2_sum = pBuffer.Value;
tag_r2_sum=reshape(tag_r2_sum, [kmax, sum(defvec)])';
clear pBuffer
end

clf;figure(1); hold on;

h2vec_str = {'0.1', '0.4', '0.7'};
pivec_str = {'1e-05', '0.0001', '0.001', '0.01'};

if 1
    % test bivariate stuff 
    %trait_name = 'simu_h2=0.4_rg=0.0_pi1u=1e-03_pi2u=1e-03_pi12=1e-03_rep=1_tag1=completePolygenicOverlap_tag2=evenPolygenicity';fig=1;
    %trait_name = 'simu_h2=0.4_rg=0.0_pi1u=1e-03_pi2u=1e-03_pi12=3e-04_rep=1_tag1=partial25PolygenicOverlap_tag2=evenPolygenicity';fig=2;
    %trait_name ='simu_h2=0.4_rg=0.0_pi1u=1e-03_pi2u=1e-03_pi12=1e-06_rep=1_tag1=randomPolygenicOverlap_tag2=evenPolygenicity'; fig=3;
    
    %trait_name_vec{1} = 'simu_h2=0.4_rg=0.0_pi1u=1e-02_pi2u=1e-02_pi12=1e-02_rep=1_tag1=completePolygenicOverlap_tag2=evenPolygenicity';fig=1;
    %trait_name_vec{2} = 'simu_h2=0.4_rg=0.0_pi1u=1e-02_pi2u=1e-02_pi12=3e-03_rep=1_tag1=partial25PolygenicOverlap_tag2=evenPolygenicity';fig=2;
    %trait_name_vec{3} ='simu_h2=0.4_rg=0.0_pi1u=1e-02_pi2u=1e-02_pi12=1e-04_rep=1_tag1=randomPolygenicOverlap_tag2=evenPolygenicity'; fig=3;
    
    trait_name_vec{1} = 'simu_h2=0.4_rg=0.0_pi1u=3e-03_pi2u=3e-03_pi12=3e-03_rep=1_tag1=completePolygenicOverlap_tag2=evenPolygenicity';fig=1;
    trait_name_vec{2} = 'simu_h2=0.4_rg=0.0_pi1u=3e-03_pi2u=3e-03_pi12=8e-04_rep=1_tag1=partial25PolygenicOverlap_tag2=evenPolygenicity';fig=2;
    trait_name_vec{3} ='simu_h2=0.4_rg=0.0_pi1u=3e-03_pi2u=3e-03_pi12=9e-06_rep=1_tag1=randomPolygenicOverlap_tag2=evenPolygenicity'; fig=3;
    
    dat_trait1 = {}; dat_trait2 = {}; pi12_frac_cost = {};
    for fig=1:3
        trait_name= trait_name_vec{fig};
        dat_trait1{fig} = load(['H:\NORSTORE\oleksanf\11015833\SIMU_BGMG2\', trait_name, '.trait1.mat']);
        dat_trait2{fig} = load(['H:\NORSTORE\oleksanf\11015833\SIMU_BGMG2\', trait_name, '.trait2.mat']);

        dat_trait1{fig}.nvec = ones(size(dat_trait1.zvec)) * 100000;
        dat_trait2{fig}.nvec = ones(size(dat_trait2.zvec)) * 100000;
        pi12_frac_cost{fig} = [];
    end
    
    figure(fig); hold on;
    pi12_frac_vec = 0:0.1:1;
    
    for pi12_frac = pi12_frac_vec
        for fig=1:3
            pi_vec = [(1-pi12_frac), (1-pi12_frac), pi12_frac] * dat_trait1{fig}.causal_pi;

            sig2_beta = [dat_trait1{fig}.sigsq, dat_trait2{fig}.sigsq];
            sig2_zero = [1.0, 1.0];
            rho_beta = 0; rho_zero = 0;
            calllib('bgmg', 'bgmg_set_zvec', 0, 1, sum(defvec), dat_trait1{fig}.zvec(defvec));  check();
            calllib('bgmg', 'bgmg_set_nvec', 0, 1, sum(defvec), dat_trait1{fig}.nvec(defvec));  check();
            calllib('bgmg', 'bgmg_set_zvec', 0, 2, sum(defvec), dat_trait2{fig}.zvec(defvec));  check();
            calllib('bgmg', 'bgmg_set_nvec', 0, 2, sum(defvec), dat_trait2{fig}.nvec(defvec));  check();

            pi12_frac_cost{fig}(end+1, 1) = calllib('bgmg', 'bgmg_calc_bivariate_cost', 0, 3, pi_vec, 2, sig2_beta, rho_beta, 2, sig2_zero, rho_zero);  check(); 
        end
    end
    
    figure(1); hold on;
    for fig=1:3
    plot(pi12_frac_vec, pi12_frac_cost{fig}, '.');
    end
    drawnow;
    end
    
    % pi12_frac_vec(1, end+1)=0.3
    end
    
    if 0
        koef_vec = logspace(-1, 1, 11);
        cost_pi_vec = [];
        for koef = koef_vec
            pi_vec = [1, 1, 0] .* koef .* dat_trait1.causal_pi;
            cost_pi_vec(end+1, 1) = calllib('bgmg', 'bgmg_calc_bivariate_cost', 0, 3, pi_vec, 2, sig2_beta, rho_beta, 2, sig2_zero, rho_zero);  check(); 
        end
        cost_pi_vec(cost_pi_vec > 1e99) = nan;
        plot(koef_vec, cost_pi_vec);
    end
    
    calllib('bgmg', 'bgmg_set_option', 0,  'diag', 0); check();

end

return

for repi=1:10
for h2_index = [2 3 1]
for pi_index = [3 2 4 1]
   subplot(3, 4, (h2_index-1)*4 + pi_index);
   title(sprintf('h2=%s, pi=%s', h2vec_str{h2_index}, pivec_str{pi_index}));
%end;end;end;

hold on
ax = gca;
ax.ColorOrderIndex = 1;
dat= load(['H:\NORSTORE\oleksanf\11015833\simu_ugmg_120_traits\', sprintf('simu_h2=%s_pi1u=%s_rep=%i.trait1.mat', h2vec_str{h2_index}, pivec_str{pi_index}, repi)]);

dat.sig2zero = 1;
zvec = dat.zvec; nvec = ones(size(zvec)) * 100000;
zvec(~isfinite(zvec)) = 100;
%sum(~isfinite(zvec(defvec)))

zvec_test = zvec; %zvec_test(abs(zvec_test) < 0.5) = nan;    % <-------------------- try idea with not fitting bad z scores
calllib('bgmg', 'bgmg_set_zvec', 0, trait, sum(defvec), zvec_test(defvec));  check();
calllib('bgmg', 'bgmg_set_nvec', 0, trait, sum(defvec), nvec(defvec));  check();

fprintf('Fitting params...\n');
model_params{1} = struct('pi_vec', dat.causal_pi, 'sig2_beta', dat.sigsq, 'sig2_zero', dat.sig2zero);
model_params{2} = BGMG_util.UGMG_mapparams1(fminsearch(@BGMG_util.UGMG_CPP_fminsearch_cost, BGMG_util.UGMG_mapparams1(struct('pi_vec', dat.causal_pi, 'sig2_beta', dat.sigsq, 'sig2_zero', 1.05)), struct('Display', 'off')));

% calculate model cdf
zgrid = single(0:0.05:15); 

model_logpvec={};
for mi = 1:2
    m=model_params{mi};
    pBuffer = libpointer('singlePtr', zeros(length(zgrid), 1, 'single'));
    %cost=calllib('bgmg', 'bgmg_calc_univariate_cost', 0, dat.causal_pi, dat.sig2zero, dat.sigsq);  check(); 
    calllib('bgmg', 'bgmg_calc_univariate_pdf', 0, m.pi_vec, m.sig2_zero, m.sig2_beta, length(zgrid), zgrid, pBuffer);  check(); 
    pdf = pBuffer.Value'; clear pBuffer
    pdf = pdf / sum(weights(defvec));
    if (zgrid(1) == 0), zgrid = [-fliplr(zgrid(2:end)) zgrid];pdf = [fliplr(pdf(2:end)) pdf]; end
    model_cdf = cumsum(pdf)  * (zgrid(2) - zgrid(1)) ;
    X = model_cdf;X1 = ones(size(X, 1), 1); X0 = zeros(size(X, 1), 1);
    model_cdf = 0.5 * ([X0, X(:, 1:(end-1))] + [X(:, 1:(end-1)), X1]);
    model_logpvec{mi} = -log10(2*interp1(-zgrid(zgrid<=0), model_cdf(zgrid<=0), zgrid(zgrid>=0))); % hv_z is to fine, can't afford calculation on it - do interpolation instead; don't matter for QQ plot (no visual difference), but lamGCfromQQ doesn't work for z_grid (to coarse)
end

% calculate data cdf        
zvec_idx = zvec(defvec); weights_idx = weights(defvec); 
weights_idx=weights_idx/sum(weights_idx);
[data_y, si] = sort(-log10(2*normcdf(-abs(zvec_idx))));
data_x=-log10(cumsum(weights_idx(si),1,'reverse'));
data_idx = ([data_y(2:end); +Inf] ~= data_y);
hv_logp = -log10(2*normcdf(-zgrid(zgrid >= 0)));
data_logpvec = interp1(data_y(data_idx), data_x(data_idx), hv_logp);

% plot QQ plots
hData        = plot(data_logpvec, hv_logp, '-', 'LineWidth',1); hold on;
hModel_true  = plot(model_logpvec{1},hv_logp, '-', 'LineWidth',1); hold on;
hModel_fit   = plot(model_logpvec{2},hv_logp, '-', 'LineWidth',1); hold on;

qq_options=[];
if ~isfield(qq_options, 'qqlimy'), qq_options.qqlimy = 20; end;
if ~isfield(qq_options, 'qqlimx'), qq_options.qqlimx = 7; end;
plot([0 qq_options.qqlimy],[0 qq_options.qqlimy], 'k--');
xlim([0 qq_options.qqlimx]); ylim([0 qq_options.qqlimy]);
drawnow
end
end
end

return

koef_vec = logspace(-1, 1, 31);
cost_pi = [];cost_sig2beta = []; cost_sig2zero = [];
for koef = koef_vec
    cost = calllib('bgmg', 'bgmg_calc_univariate_cost', 0, dat.causal_pi * koef, dat.sig2zero, dat.sigsq);  check(); fprintf('%.3f\t', cost); cost_pi(end+1, 1) = cost;
    cost = calllib('bgmg', 'bgmg_calc_univariate_cost', 0, dat.causal_pi, dat.sig2zero * koef, dat.sigsq);  check(); fprintf('%.3f\t', cost); cost_sig2zero(end+1, 1) = cost;
    cost = calllib('bgmg', 'bgmg_calc_univariate_cost', 0, dat.causal_pi, dat.sig2zero, dat.sigsq * koef);  check(); fprintf('%.3f\n', cost); cost_sig2beta(end+1, 1) = cost;
end
cost_pi(cost_pi > 1e99) = nan;
figure(2);hold on; subplot(1,3,1); plot(log10(koef_vec), cost_pi)
figure(2);hold on; subplot(1,3,2); plot(log10(koef_vec), cost_sig2beta)
figure(2);hold on; subplot(1,3,3); plot(log10(koef_vec), cost_sig2zero)


dat.causal_pi = exp(f_opt(1));
dat.sig2zero = f_opt(2);
dat.sigsq = exp(f_opt(3));