#include "conditional.h"

/*
 * cond_analysis constructor
 */
cond_analysis::cond_analysis(double p_cutoff, double collinear, double ld_window, string out, bool verbose, double top_snp, double freq_thres, string name)
{
	cname = name;
	a_out = out;
	a_p_cutoff = p_cutoff;
	a_collinear = collinear;
	a_ld_window = ld_window;
	a_verbose = verbose;
	a_freq_threshold = freq_thres;

	a_top_snp = (top_snp < 0 ? 1e10 : top_snp);

	num_snps = 0;
}

/*
 * cond_analysis default constructor
 */
cond_analysis::cond_analysis()
{
	cname = "conditional-default";
	a_out = "result";
	a_p_cutoff = 5e-8;
	a_collinear = 0.9;
	a_ld_window = 1e7;
	a_verbose = true;
	a_freq_threshold = 0.2;

	a_top_snp = -1;

	num_snps = 0;
}

/*
 * Initialise the conditional analysis by matching SNPs
 * and calculating frequencies. 
 * From `gcta::init_massoc`.
 */
void cond_analysis::init_conditional(phenotype *pheno, reference *ref)
{
	size_t i = 0, j = 0;
	size_t n, m;

	// First match the datasets
	match_gwas_phenotype(pheno, ref);
	// Re-caculate variance after matching to reference
	//jma_Vp = jma_Ve = pheno->calc_variance();
	jma_Vp = jma_Ve = pheno->get_variance();

	n = to_include.size();
	m = fam_ids_inc.size();

	msx_b.resize(n);
	nD.resize(n);

#pragma omp parallel for
	for (i = 0; i < n; i++) {
		eigenVector x;
		makex_eigenVector(i, x, true, ref);
		msx_b[i] = x.squaredNorm() / (double)m;
	}

	msx = 2.0 * ja_freq.array() * (1.0 - ja_freq.array());

	for (i = 0; i < n; i++) {
		nD[i] = (jma_Vp - msx[i] * ja_beta[i] * ja_beta[i]) / (msx[i] * ja_beta_se[i] * ja_beta_se[i]) + 1;
	}
}

/*
 * Matches reference SNPs to phenotype SNPs
 * @ret void
 */
void cond_analysis::match_gwas_phenotype(phenotype *pheno, reference *ref)
{
	size_t i = 0;
	map<string, size_t>::iterator iter;
	map<string, size_t> id_map;
	vector<size_t> idx, pheno_idx;
	vector<string> snps;
	unsigned int unmatched = 0;

	to_include.clear();
	ref->includes_clear();

	// Match GWAS data to reference data and initialise the inclusion list
	//ref->update_inclusion(pheno->matched_idx, pheno->snp_name);
	for (i = 0; i < pheno->snp_name.size(); i++) {
		if ((iter = ref->snp_map.find(pheno->snp_name[i])) == ref->snp_map.end()
			|| (pheno->allele1[i] != ref->bim_allele1[iter->second] && (pheno->allele1[i] != ref->bim_allele2[iter->second]))
			)
		{
			continue;
		}
		pheno_idx.push_back(i);
		id_map.insert(pair<string, size_t>(pheno->snp_name[i], i));
		snps.push_back(iter->first);
		idx.push_back(iter->second);
	}
	ref->update_inclusion(idx, snps);
	snps.clear();
	idx.clear();

	// Allele matching and swapping
	mu = ref->mu; // Copy across as this will be morphed below
	for (i = 0; i < ref->to_include.size(); i++) {
		bool flip_allele = false;
		iter = id_map.find(ref->bim_snp_name[ref->to_include[i]]);
		if (iter == id_map.end())
			continue;

		ref->ref_A[ref->to_include[i]] = pheno->allele1[iter->second];

		if (!ref->mu.empty() && pheno->allele1[iter->second] == ref->bim_allele2[ref->to_include[i]]) {
			mu[ref->to_include[i]] = 2.0 - ref->mu[ref->to_include[i]];
			flip_allele = true;
		}
		else {
			mu[ref->to_include[i]] = ref->mu[ref->to_include[i]];
		}

		double cur_freq = mu[ref->to_include[i]] / 2.0;
		double freq_diff = abs(cur_freq - pheno->freq[iter->second]);
		if (
			//(ref->bim_allele1[ref->to_include[i]] == pheno->allele1[iter->second]
			//	|| ref->bim_allele2[ref->to_include[i]] == pheno->allele1[iter->second]
			//) &&
			freq_diff < a_freq_threshold) 
		{
			snps.push_back(iter->first);
			idx.push_back(iter->second);
		}
		else {
			unmatched++;
		}
	}

	if (unmatched) {
		cout << "[" << pheno->get_phenoname() << "] There were " << unmatched << " SNPs that had a large difference in the allele frequency to that of the reference sample." << endl;
	}

	ref->update_inclusion(idx, snps);
	to_include = ref->to_include;
	fam_ids_inc = ref->fam_ids_inc;

	if (to_include.empty()) {
		ShowError("Included list of SNPs is empty - could not match SNPs from phenotype file with reference SNPs.");
	}
	else {
		cout << "[" << cname << "] Total amount of SNPs matched from phenotype file with reference SNPs are: " << to_include.size() << endl;
	}

	// Resize and get ready for the conditional analysis
	ja_snp_name.resize(to_include.size());
	ja_freq.resize(to_include.size());
	ja_beta.resize(to_include.size());
	ja_beta_se.resize(to_include.size());
	ja_pval.resize(to_include.size());
	ja_chisq.resize(to_include.size());
	ja_N_outcome.resize(to_include.size());

	for (i = 0; i < to_include.size(); i++) {
		ja_snp_name[i] = pheno->snp_name[idx[i]];
		ja_freq[i] = pheno->freq[idx[i]];
		ja_beta[i] = pheno->beta[idx[i]];
		ja_beta_se[i] = pheno->se[idx[i]];
		//ja_pval[i] = pheno->pval[idx[i]];
		ja_chisq[i] = (ja_beta[i] / ja_beta_se[i]) * (ja_beta[i] / ja_beta_se[i]);
		ja_pval[i] = pchisq(ja_chisq[i], 1.0);
		ja_N_outcome[i] = pheno->n[idx[i]];
	}

	for (i = 0; i < to_include.size(); i++) {
		string filename = a_out + "." + pheno->get_phenoname() + ".badsnps";
		ofstream file(filename.c_str());

		file << "SNP\tChisq\tPval\tFreq" << endl;
		for (i = 0; i < ja_snp_name.size(); i++) {
			file << ja_snp_name[i] << "\t" << ja_chisq[i] << "\t" << ja_pval[i] << "\t" << ja_freq[i] << endl;
		}
		file.close();
	}
}

void cond_analysis::stepwise_select(vector<size_t> &selected, vector<size_t> &remain, eigenVector &bC, eigenVector &bC_se, eigenVector &pC, reference *ref)
{
	vector<double> p_temp, chisq;
	eigenVector2Vector(ja_pval, p_temp);
	eigenVector2Vector(ja_chisq, chisq);
	size_t i = 0, prev_num = 0,
		//m = min_element(p_temp.begin(), p_temp.end()) - p_temp.begin();
		m = max_element(chisq.begin(), chisq.end()) - chisq.begin();;

	cout << "[" << cname << "] Selected SNP " << ja_snp_name[m] << " with chisq " << ja_chisq[m] << " and pval " << ja_pval[m] << endl;
	if (ja_pval[m] >= a_p_cutoff) {
		cout << "[" << cname << "] SNP did not meet threshold." << endl;
		return;
	}
	selected.push_back(m);

	for (i = 0; i < to_include.size(); i++) {
		if (i != m)
			remain.push_back(i);
	}

	if (a_p_cutoff > 1e-3) {
		ShowWarning("P value level is too low for stepwise model.", a_verbose);
	}

	while (!remain.empty()) {
		if (select_entry(selected, remain, bC, bC_se, pC, ref)) {
			selected_stay(selected, bC, bC_se, pC, ref);
		}
		else
			break;
		if (selected.size() % 5 == 0 && selected.size() > prev_num)
			cout << "[" << cname << "] " << selected.size() << " associated SNPs have been selected." << endl;
		if (selected.size() > prev_num)
			prev_num = selected.size();
		if (selected.size() >= a_top_snp)
			break;
	}

	if (a_p_cutoff > 1e-3) {
		cout << "Performing backward elimination..." << endl;
		selected_stay(selected, bC, bC_se, pC, ref);
	}

	cout << "[" << cname << "] Finally, " << selected.size() << " associated SNPs have been selected." << endl;
}

bool cond_analysis::insert_B_Z(const vector<size_t> &idx, size_t pos, reference *ref)
{
	bool get_ins_col = false, get_ins_row = false;
	size_t i = 0, j = 0, p,
		n = fam_ids_inc.size(),
		m = to_include.size();
	double d_temp = 0.0;
	vector<size_t> ix(idx);
	eigenSparseMat B_temp(B), B_N_temp(B_N);

	ix.push_back(pos);
	stable_sort(ix.begin(), ix.end());

	B.resize(ix.size(), ix.size());
	B_N.resize(ix.size(), ix.size());

	p = find(ix.begin(), ix.end(), pos) - ix.begin();
	eigenVector diagB(ix.size());
	eigenVector x_i(n), x_j(n);
	for (j = 0; j < ix.size(); j++) {
		B.startVec(j);
		B_N.startVec(j);
		B.insertBack(j, j) = msx_b[ix[j]];
		B_N.insertBack(j, j) = msx[ix[j]] * nD[ix[j]];

		diagB[j] = msx_b[ix[j]];
		if (pos == ix[j]) {
			get_ins_col = true;
		}
		get_ins_row = get_ins_col;
		makex_eigenVector(ix[j], x_j, false, ref);
		
		for (i = j + 1; i < ix.size(); i++) {
			if (pos == ix[i])
				get_ins_row = true;

			if (pos == ix[j] || pos == ix[i]) {
				if ((ref->bim_chr[to_include[ix[i]]] == ref->bim_chr[to_include[ix[j]]]
						&& abs(ref->bim_bp[to_include[ix[i]]] - ref->bim_bp[to_include[ix[j]]]) < a_ld_window)
					)
				{
					makex_eigenVector(ix[i], x_i, false, ref);
					d_temp = x_i.dot(x_j) / (double)n;
					B.insertBack(i, j) = d_temp;
					B_N.insertBack(i, j) = d_temp
											* min(nD[ix[i]], nD[ix[j]])
											* sqrt(msx[ix[i]] * msx[ix[j]] / (msx_b[ix[i]] * msx_b[ix[j]]));
				}
			}
			else {
				size_t ins_row_val = get_ins_row ? 1 : 0,
					ins_col_val = get_ins_col ? 1 : 0;
				if (B_temp.coeff(i - ins_row_val, j - ins_col_val) != 0) {
					B.insertBack(i, j) = B_temp.coeff(i - ins_row_val, j - ins_col_val);
					B_N.insertBack(i, j) = B_N_temp.coeff(i - ins_row_val, j - ins_col_val);
				}
			}
		}
	}
	B.finalize();
	B_N.finalize();

	SimplicialLDLT<eigenSparseMat> ldlt_B(B);
	B_i.resize(ix.size(), ix.size());
	B_i.setIdentity();
	B_i = ldlt_B.solve(B_i).eval();
	if (ldlt_B.vectorD().minCoeff() < 0 || sqrt(ldlt_B.vectorD().maxCoeff() / ldlt_B.vectorD().minCoeff()) > 30
		|| (1 - eigenVector::Constant(ix.size(), 1).array() / (diagB.array() * B_i.diagonal().array())).maxCoeff() > a_collinear)
	{
		jma_snpnum_collinear++;
		B = B_temp;
		B_N = B_N_temp;
		return false;
	}

	SimplicialLDLT<eigenSparseMat> ldlt_B_N(B_N);
	B_N_i.resize(ix.size(), ix.size());
	B_N_i.setIdentity();
	B_N_i = ldlt_B_N.solve(B_N_i).eval();
	D_N.resize(ix.size());
	for (j = 0; j < ix.size(); j++) {
		D_N[j] = msx[ix[j]] * nD[ix[j]];
	}

	if (Z_N.cols() < 1)
		return true;

	eigenSparseMat Z_temp(Z), Z_N_temp(Z_N);
	Z.resize(ix.size(), m);
	Z_N.resize(ix.size(), m);

	for (j = 0; j < m; j++) {
		Z.startVec(j);
		Z_N.startVec(j);

		get_ins_row = false;
		makex_eigenVector(j, x_j, false, ref);
		for (i = 0; i < ix.size(); i++) {
			if (pos == ix[i]) {
				if ((ix[i] != j 
						&& ref->bim_chr[to_include[ix[i]]] == ref->bim_chr[to_include[j]]
						&& abs(ref->bim_bp[to_include[ix[i]]] - ref->bim_bp[to_include[j]]) < a_ld_window)
					)
				{
					makex_eigenVector(ix[i], x_i, false, ref);
					d_temp = x_j.dot(x_i) / (double)n;
					Z.insertBack(i, j) = d_temp;
					Z_N.insertBack(i, j) = d_temp
											* min(nD[ix[i]], nD[j])
											* sqrt(msx[ix[i]] * msx[j] / (msx_b[ix[i]] * msx_b[j]));
				}
				get_ins_row = true;
			}
			else {
				size_t ins_row_val = get_ins_row ? 1 : 0;
				if (Z_temp.coeff(i - ins_row_val, j) != 0) {
					Z.insertBack(i, j) = Z_temp.coeff(i - ins_row_val, j);
					Z_N.insertBack(i, j) = Z_N_temp.coeff(i - ins_row_val, j);
				}
			}
		}
	}

	Z.finalize();
	Z_N.finalize();
	return true;
}

void cond_analysis::erase_B_and_Z(const vector<size_t> &idx, size_t erase)
{
	bool get_ins_col = false, get_ins_row = false;
	size_t i = 0, j = 0,
		i_size = idx.size(),
		pos = find(idx.begin(), idx.end(), erase) - idx.begin(),
		m = to_include.size();
	eigenSparseMat B_dense(B), B_N_dense(B_N);

	B.resize(i_size - 1, i_size - 1);
	B_N.resize(i_size - 1, i_size - 1);
	D_N.resize(i_size - 1);

	for (j = 0; j < i_size; j++) {
		if (erase == idx[j]) {
			get_ins_col = true;
			continue;
		}

		B.startVec(j - get_ins_col);
		B_N.startVec(j - get_ins_col);
		D_N[j - (get_ins_col ? 1 : 0)] = msx[idx[j]] * nD[idx[j]];
		get_ins_row = get_ins_col;

		for (i = j; i < i_size; i++) {
			if (erase == idx[i]) {
				get_ins_row = true;
				continue;
			}

			if (B_dense.coeff(i, j) != 0) {
				size_t ins_row_val = get_ins_row ? 1 : 0,
					ins_col_val = get_ins_col ? 1 : 0;
				B.insertBack(i - ins_row_val, j - ins_col_val) = B_dense.coeff(i, j);
				B_N.insertBack(i - ins_row_val, j - ins_col_val) = B_N_dense.coeff(i, j);
			}
		}
	}
	B.finalize();
	B_N.finalize();

	if (Z_N.cols() < 1)
		return;

	SimplicialLDLT<eigenSparseMat> ldlt_B(B);
	B_i.resize(i_size - 1, i_size - 1);
	B_i.setIdentity();

	B_i = ldlt_B.solve(B_i).eval();
	SimplicialLDLT<eigenSparseMat> ldlt_B_N(B_N);
	B_N_i.resize(i_size - 1, i_size - 1);
	B_N_i.setIdentity();
	B_N_i = ldlt_B_N.solve(B_N_i).eval();

	eigenSparseMat Z_temp(Z), Z_N_temp(Z_N);
	Z.resize(i_size - 1, m);
	Z_N.resize(i_size - 1, m);
	for (j = 0; j < m; j++) {
		Z.startVec(j);
		Z_N.startVec(j);
		get_ins_row = false;
		for (i = 0; i < i_size; i++) {
			if (erase == idx[i]) {
				get_ins_row = true;
				continue;
			}

			if (Z_temp.coeff(i, j) != 0) {
				size_t ins_row_val = get_ins_row ? 1 : 0;
				Z.insertBack(i - ins_row_val, j) = Z_temp.coeff(i, j);
				Z_N.insertBack(i - ins_row_val, j) = Z_N_temp.coeff(i, j);
			}
		}
	}
	Z.finalize();
	Z_N.finalize();
}

bool cond_analysis::select_entry(vector<size_t> &selected, vector<size_t> &remain, eigenVector &bC, eigenVector &bC_se, eigenVector &pC, reference *ref)
{
	size_t m = 0;
	vector<double> pC_temp;

	massoc_conditional(selected, remain, bC, bC_se, pC, ref);

	eigenVector2Vector(pC, pC_temp);

	while (true) {
		m = min_element(pC_temp.begin(), pC_temp.end()) - pC_temp.begin();
		cout << "[" << cname << "] Selected entry SNP " << ja_snp_name[m] << " with cpval " << pC_temp[m] << endl;
		if (pC_temp[m] >= a_p_cutoff) {
			cout << "[" << cname << "] " << ja_snp_name[m] << " does not meet threshold" << endl;
			return false;
		}

		if (insert_B_Z(selected, remain[m], ref)) {
			selected.push_back(remain[m]);
			stable_sort(selected.begin(), selected.end());
			remain.erase(remain.begin() + m);
			return true;
		}

		pC_temp.erase(pC_temp.begin() + m);
		remain.erase(remain.begin() + m);
	}
}

void cond_analysis::selected_stay(vector<size_t> &select, eigenVector &bJ, eigenVector &bJ_se, eigenVector &pJ, reference *ref)
{
	if (B_N.cols() < 1) {
		if (!init_b(select, ref)) {
			ShowError("Stepwise Selection Error: There is a collinearity problem with the given list of SNPs.");
		}
	}

	vector<double> pJ_temp;
	while (!select.empty()) {
		massoc_joint(select, bJ, bJ_se, pJ, ref);
		eigenVector2Vector(pJ, pJ_temp);
		size_t m = max_element(pJ_temp.begin(), pJ_temp.end()) - pJ_temp.begin();
		if (pJ[m] > a_p_cutoff) {
			jma_snpnum_backward++;
			erase_B_and_Z(select, select[m]);
			select.erase(select.begin() + m);
			cout << "[" << cname << "] Erasing SNP " << ja_snp_name[m] << endl;
		}
		else {
			break;
		}
	}
}

void cond_analysis::massoc_conditional(const vector<size_t> &selected, vector<size_t> &remain, eigenVector &bC, eigenVector &bC_se, eigenVector &pC, reference *ref)
{
	size_t i = 0, j = 0, n = selected.size(), m = remain.size();
	double chisq = 0.0, B2 = 0.0;
	eigenVector b(n), se(n);

	if (B_N.cols() < 1) {
		if (!init_b(selected, ref)) {
			ShowError("Conditional Error: There is a collinearity problem with the SNPs given.\n");
		}
	}

	if (Z_N.cols() < 1) {
		init_z(selected, ref);
	}

	for (i = 0; i < n; i++) {
		b[i] = ja_beta[selected[i]];
		se[i] = ja_beta_se[selected[i]];
	}
	eigenVector bJ1 = B_N_i * D_N.asDiagonal() * b;

	eigenVector Z_Bi(n), Z_Bi_temp(n);
	bC = eigenVector::Zero(m);
	bC_se = eigenVector::Zero(m);
	pC = eigenVector::Constant(m, 2);
	for (i = 0; i < m; i++) {
		j = remain[i];
		B2 = msx[j] * nD[j];
		if (!isFloatEqual(B2, 0.0)) {
			Z_Bi = Z_N.col(j).transpose() * B_N_i;
			Z_Bi_temp = Z.col(j).transpose() * B_i;
			if (Z.col(j).dot(Z_Bi_temp) / msx_b[j] < a_collinear) {
				bC[i] = ja_beta[j] - Z_Bi.cwiseProduct(D_N).dot(b) / B2;
				bC_se[i] = 1.0 / B2; //(B2 - Z_N.col(j).dot(Z_Bi)) / (B2 * B2);
			}
		}
		bC_se[i] *= jma_Ve;
		if (bC_se[i] > 1e-10 * jma_Vp) {
			bC_se[i] = sqrt(bC_se[i]);
			chisq = bC[i] / bC_se[i];
			pC[i] = pchisq(chisq * chisq, 1);
		}
	}
}

double cond_analysis::massoc_calcu_Ve(const vector<size_t> &selected, eigenVector &bJ, eigenVector &b)
{
	double Ve = 0.0, d_temp = 0.0;
	size_t n = bJ.size();
	vector<double> nD_temp(n);

	for (size_t k = 0; k < n; k++) {
		nD_temp[k] = nD[selected[k]];
		Ve += D_N[k] * bJ[k] * b[k];
	}

	d_temp = v_calc_median(nD_temp);
	if (d_temp - n < 1) {
		ShowError("DoF Error: Model is over-fitting due to lack of degree of freedom. Provide a more stringent P-value cutoff.");
	}
	Ve = ((d_temp - 1) * jma_Vp - Ve) / (d_temp - n);
	if (Ve <= 0.0) {
		ShowError("Residual Error: Residual variance is out of bounds meaning the model is over-fitting. Provide a more stringent P-value cutoff.");
	}
	return Ve;
}

void cond_analysis::makex_eigenVector(size_t j, eigenVector &x, bool resize, reference *ref)
{
	size_t i = 0,
		n = fam_ids_inc.size(),
		m = to_include.size();

	if (resize)
		x.resize(n);

#pragma omp parallel for
	for (i = 0; i < n; i++) {
		if (!ref->bed_snp_1[to_include[j]][fam_ids_inc[i]] || ref->bed_snp_2[to_include[j]][fam_ids_inc[i]])
		{
			double snp1 = ref->bed_snp_1[to_include[j]][fam_ids_inc[i]] ? 1.0 : 0.0,
				snp2 = ref->bed_snp_2[to_include[j]][fam_ids_inc[i]] ? 1.0 : 0.0;
			if (ref->bim_allele1[to_include[j]] == ref->ref_A[to_include[j]])
				x[i] = snp1 + snp2;
			else
				x[i] = 2.0 - (snp1 + snp2);
		}
		else {
			x[i] = mu[to_include[j]];
		}
		x[i] -= mu[to_include[j]];
	}
}

bool cond_analysis::init_b(const vector<size_t> &idx, reference *ref)
{
	size_t i = 0, j = 0, k = 0,
		n = fam_ids_inc.size(),
		i_size = idx.size();
	double d_temp = 0.0;
	eigenVector diagB(i_size),
		x_i(n),
		x_j(n);

	B.resize(i_size, i_size);
	B_N.resize(i_size, i_size);
	D_N.resize(i_size);

	for (i = 0; i < i_size; i++) {
		D_N[i] = msx[idx[i]] * nD[idx[i]];
		B.startVec(i);
		B.insertBack(i, i) = msx_b[idx[i]];
		
		B_N.startVec(i);
		B_N.insertBack(i, i) = D_N[i];

		diagB[i] = msx_b[idx[i]];
		makex_eigenVector(idx[i], x_i, false, ref);

		for (j = i + 1; j < i_size; j++) {
			if ((ref->bim_chr[to_include[idx[i]]] == ref->bim_chr[to_include[idx[j]]]
					&& abs(ref->bim_bp[to_include[idx[i]]] - ref->bim_bp[to_include[idx[j]]]) < a_ld_window)
				)
			{
				makex_eigenVector(idx[j], x_j, false, ref);

				d_temp = x_i.dot(x_j) / (double)n;
				B.insertBack(j, i) = d_temp;
				B_N.insertBack(j, i) = d_temp 
									* min(nD[idx[i]], nD[idx[j]]) 
									* sqrt(msx[idx[i]] * msx[idx[j]] / (msx_b[idx[i]] * msx_b[idx[j]]));
			}
		}
	}

	B.finalize();
	B_N.finalize();

	SimplicialLDLT<eigenSparseMat> ldlt_B(B);
	if (ldlt_B.vectorD().minCoeff() < 0 || sqrt(ldlt_B.vectorD().maxCoeff() / ldlt_B.vectorD().minCoeff()) > 30)
		return false;
	B_i.resize(i_size, i_size);
	B_i.setIdentity();
	B_i = ldlt_B.solve(B_i).eval();
	if ((1 - eigenVector::Constant(i_size, 1).array() / (diagB.array() * B_i.diagonal().array())).maxCoeff() > a_collinear)
		return false;

	SimplicialLDLT<eigenSparseMat> ldlt_B_N(B_N);
	B_N_i.resize(i_size, i_size);
	B_N_i.setIdentity();
	B_N_i = ldlt_B_N.solve(B_N_i).eval();
	return true;
}

void cond_analysis::init_z(const vector<size_t> &idx, reference *ref)
{
	size_t i = 0, j = 0,
		n = fam_ids_inc.size(),
		m = to_include.size(),
		i_size = idx.size();
	double d_temp = 0.0;
	eigenVector x_i(n), x_j(n);

	Z.resize(i_size, m);
	Z_N.resize(i_size, m);

	for (j = 0; j < m; j++) {
		Z.startVec(j);
		Z_N.startVec(j);

		makex_eigenVector(j, x_j, false, ref);
		for (i = 0; i < i_size; i++) {
			if ((idx[i] != j 
					&& ref->bim_chr[to_include[idx[i]]] == ref->bim_chr[to_include[j]]
					&& abs(ref->bim_bp[to_include[idx[i]]] - ref->bim_bp[to_include[j]]) < a_ld_window)
				)
			{
				makex_eigenVector(idx[i], x_i, false, ref);

				d_temp = x_j.dot(x_i) / (double)n;
				Z.insertBack(i, j) = d_temp;
				Z_N.insertBack(i, j) = d_temp
										* min(nD[idx[i]], nD[j])
										* sqrt(msx[idx[i]] * msx[j] / (msx_b[idx[i]] * msx_b[j]));
			}
		}
	}

	Z.finalize();
	Z_N.finalize();
}

void cond_analysis::massoc_joint(const vector<size_t> &idx, eigenVector &bJ, eigenVector &bJ_se, eigenVector &pJ, reference *ref)
{
	size_t i = 0, n = idx.size();
	double chisq = 0.0;
	eigenVector b(n);
	for (i = 0; i < n; i++)
		b[i] = ja_beta[idx[i]];

	if (B_N.cols() < 1) {
		if (!init_b(idx, ref))
			ShowError("`massoc_joint`: There is a collinearity problem with the given list of SNPs.");
	}
	
	bJ.resize(n);
	bJ_se.resize(n);
	pJ.resize(n);
	bJ = B_N_i * D_N.asDiagonal() * b;
	bJ_se = B_N_i.diagonal();
	pJ = eigenVector::Ones(n);
	bJ_se *= jma_Ve;
	for (i = 0; i < n; i++) {
		if (bJ_se[i] > 1.0e-30) {
			bJ_se[i] = sqrt(bJ_se[i]);
			chisq = bJ[i] / bJ_se[i];
			pJ[i] = pchisq(chisq * chisq, 1.0);
		}
		else {
			bJ[i] = 0.0;
			bJ_se[i] = 0.0;
		}
	}
}

vector<size_t> cond_analysis::read_snplist(string snplist, vector<size_t> &remain, reference *ref)
{
	size_t i = 0, n = to_include.size();
	vector<string> givenSNPs;
	vector<size_t> pgiven;
	string temp;
	ifstream i_snplist(snplist.c_str());
	
	// Read from file
	givenSNPs.clear();
	if (!i_snplist) {
		ShowError("IO Error: Cannot read " + snplist + " to read SNP list.");
	}
	cout << "Reading SNPs upon which to condition from " + snplist + "." << endl;
	while (i_snplist >> temp) {
		givenSNPs.push_back(temp);
		getline(i_snplist, temp);
	}
	i_snplist.close();
	if (givenSNPs.empty()) {
		ShowError("No SNPs were read from the SNP list file - please check the format of this file.");
	}

	map<string, int> m_gSNPs;
	size_t snps_size = givenSNPs.size();
	pgiven.clear();
	remain.clear();
	for (i = 0; i < snps_size; i++) {
		m_gSNPs.insert(pair<string, int>(givenSNPs[i], static_cast<int>(i)));
	}
	for (i = 0; i < n; i++) {
		if (m_gSNPs.find(ref->bim_snp_name[to_include[i]]) != m_gSNPs.end()) {
			pgiven.push_back(i);
		}
		else {
			remain.push_back(i);
		}
	}
	if (pgiven.size() > 0) {
		cout << pgiven.size() << " conditional SNP(s) were matched to the reference dataset." << endl;
	}
	else {
		ShowError("None of the SNPs from the SNP list could be matched. Please double check the datasets.");
	}
	return pgiven;
}

/*
 * Determine number of independent association signals within the region
 * without conducting a conditional analysis.
 */
void cond_analysis::find_independent_snps(reference *ref)
{
	vector<size_t> selected, remain;
	eigenVector bC, bC_se, pC;

	if (a_top_snp <= 0.0)
		a_top_snp = 1e10;

	cout << "[" << cname << "] Performing stepwise model selection on " << to_include.size() << " SNPs; p cutoff = " << a_p_cutoff << ", collinearity = " << a_collinear << " assuming complete LE between SNPs more than " << a_ld_window / 1e6 << " Mb away)." << endl;
	stepwise_select(selected, remain, bC, bC_se, pC, ref);

	if (selected.empty()) {
		ShowError("Conditional Error: No SNPs have been selected by the step-wise selection algorithm.");
	}
	else if (selected.size() >= fam_ids_inc.size()) {
		ShowError("Conditional Error: Too many SNPs. The number of SNPs should not be larger than the sample size.");
	}

	cout << "[" << cname << "] (" << jma_snpnum_backward << " SNPs eliminated by backward selection.)" << endl;
	sanitise_output(selected, bC, bC_se, pC, ref);

	num_ind_snps = selected.size();
	ind_snps = selected;
	remain_snps = remain;

	// Lazy - find a new way to do this
	B_master = B;
	B_i_master = B_i;
	B_N_master = B_N;
	B_N_i_master = B_N_i;
	D_N_master = D_N;
	Z_master = Z;
	Z_N_master = Z_N;
}

/*
 * Run step-wise selection to find independent association signals/SNP
 */
void cond_analysis::pw_conditional(int pos, reference *ref)
{
	vector<size_t> selected, remain;
	eigenVector bC, bC_se, pC;

	selected = ind_snps;
	remain = remain_snps;
	B = B_master;
	B_i = B_i_master;
	B_N = B_N_master;
	B_N_i = B_N_i_master;
	D_N = D_N_master;
	Z = Z_master;
	Z_N = Z_N_master;

	// Exclude SNP from conditional
	if (pos >= 0) {
		pos = (size_t)pos;
		remain.push_back(selected[pos]);
		erase_B_and_Z(selected, selected[pos]);
		selected.erase(selected.begin() + pos); // Expensive!
	}

	massoc_conditional(selected, remain, bC, bC_se, pC, ref);

	// Save in friendly format for mdata class
	snps_cond.clear();
	b_cond.clear();
	se_cond.clear();
	maf_cond.clear();
	p_cond.clear();
	n_cond.clear();

	for (size_t i = 0; i < selected.size(); i++) {
		size_t j = selected[i];
		snps_cond.push_back(ref->bim_snp_name[to_include[j]]);
		b_cond.push_back(ja_beta[j]);
		se_cond.push_back(ja_beta_se[j]);
		maf_cond.push_back(ja_freq[j]);
		p_cond.push_back(ja_pval[j]);
		n_cond.push_back(nD[j]);
	}

	for (size_t i = 0; i < remain.size(); i++) {
		size_t j = remain[i];
		snps_cond.push_back(ref->bim_snp_name[to_include[j]]);
		b_cond.push_back(ja_beta[j]);
		se_cond.push_back(ja_beta_se[j]);
		maf_cond.push_back(ja_freq[j]);
		p_cond.push_back(ja_pval[j]);
		n_cond.push_back(nD[j]);
	}
	cond_passed = bC.size() > 0;

	// Perform the joint analysis 
	//eigenVector bJ, bJ_se, pJ;
	//massoc_joint(selected, bJ, bJ_se, pJ, ref);

	//eigenMatrix rval(selected.size(), selected.size());
	//LD_rval(selected, rval);
	
	//sanitise_output(remain, bC, bC_se, pC, rval, CO_COND, ref);
	//sanitise_output(selected, bJ, bJ_se, pJ, rval, CO_JOINT, ref);
}

void cond_analysis::LD_rval(const vector<size_t> &idx, eigenMatrix &rval)
{
	size_t i = 0, j = 0,
		i_size = idx.size();
	eigenVector sd(i_size);

	for (i = 0; i < i_size; i++)
		sd[i] = sqrt(msx_b[idx[i]]);

	for (j = 0; j < i_size; j++) {
		rval(j, j) = 1.0;
		for (i = j + 1; i < i_size; i++) {
			rval(i, j) = rval(j, i) = B.coeff(i, j) / sd[i] / sd[j];
		}
	}
}

void cond_analysis::sanitise_output(vector<size_t> &selected, eigenVector &bJ, eigenVector &bJ_se, eigenVector &pJ, reference *ref)
{
	string filename = cname + ".cma.cojo";
	ofstream ofile(filename.c_str());
	size_t i = 0, j = 0;
	
	if (!ofile)
		ShowError("Cannot open file \"" + filename + "\" for writing.");

	// Header
	ofile << "Chr\tSNP\tbp\trefA\tfreq\tb\tse\tp\tn\tfreq_geno\tbC\tbC_se\tpC";
	ofile << endl;

	snps_cond.resize(selected.size());
	for (i = 0; i < selected.size(); i++) {
		j = selected[i];
		ofile << ref->bim_chr[to_include[j]] << "\t" << ref->bim_snp_name[to_include[j]] << "\t" << ref->bim_bp[to_include[j]] << "\t";
		ofile << ref->ref_A[to_include[j]] << "\t" << ja_freq[j] << "\t" << ja_beta[j] << "\t" << ja_beta_se[j] << "\t";
		ofile << ja_pval[j] << "\t" << nD[j] << "\t" << 0.5 * mu[to_include[j]] << "\t";

		ofile << bJ[i] << "\t" << bJ_se[i] << "\t" << pJ[i] << "\t";
		ofile << 0 << endl;
	}
	ofile.close();
}

/*
 * Initialise matched data class from two conditional analyses
 */
mdata::mdata(cond_analysis *ca1, cond_analysis *ca2)
{
	if (!ca1->coloc_ready() || !ca2->coloc_ready()) {
		return; // TODO Handle me
	}

	size_t n = ca1->snps_cond.size();
	vector<string>::iterator it;

	// Match SNPs
	for (it = ca1->snps_cond.begin(); it != ca1->snps_cond.end(); it++) {
		vector<string>::iterator it2;
		if ((it2 = find(ca2->snps_cond.begin(), ca2->snps_cond.end(), *it)) != ca2->snps_cond.end()) {
			snp_map.insert(pair<size_t, size_t>(distance(ca1->snps_cond.begin(), it), distance(ca2->snps_cond.begin(), it2)));
		}
	}

	// Extract data we want
	map<size_t, size_t>::iterator itmap = snp_map.begin();
	size_t m = snp_map.size();
	while (itmap != snp_map.end()) {
		snps1.push_back(ca1->snps_cond[itmap->first]);
		betas1.push_back(ca1->b_cond[itmap->first]);
		ses1.push_back(ca1->se_cond[itmap->first]);
		pvals1.push_back(ca1->p_cond[itmap->first]);
		mafs1.push_back(ca1->maf_cond[itmap->first]);
		ns1.push_back(ca1->n_cond[itmap->first]);

		snps2.push_back(ca2->snps_cond[itmap->second]);
		betas2.push_back(ca2->b_cond[itmap->second]);
		ses2.push_back(ca2->se_cond[itmap->second]);
		pvals2.push_back(ca2->p_cond[itmap->second]);
		mafs2.push_back(ca2->maf_cond[itmap->second]);
		ns2.push_back(ca2->n_cond[itmap->second]);

		itmap++;
	}
}
