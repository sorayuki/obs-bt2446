#include <math.h>
#include <obs-module.h>

#include "bt2446-resources.h"

#define METHOD_A 0
#define METHOD_B 1
#define METHOD_C 2

/* Prefer a data-directory copy so a broken shader can be fixed without a
 * rebuild, but fall back to the embedded source when the host provides no
 * module data directory. */
static gs_effect_t *bt2446_load_effect(void)
{
	char *effect_path = obs_module_file("bt2446.effect");
	char *error = NULL;
	gs_effect_t *effect = NULL;

	if (effect_path) {
		effect = gs_effect_create_from_file(effect_path, &error);
		bfree(effect_path);
	}

	if (!effect) {
		if (error) {
			blog(LOG_ERROR, "[sorayuki-bt2446-tonemap] effect '%s' failed to parse: %s",
			     "bt2446.effect", error);
			bfree(error);
			error = NULL;
		}
		/* BT2446_EFFECT_SOURCE is generated as a zero-terminated byte array. */
		effect = gs_effect_create((const char *)BT2446_EFFECT_SOURCE,
					  "bt2446.effect", &error);
		if (!effect) {
			blog(LOG_ERROR, "[sorayuki-bt2446-tonemap] embedded effect failed to compile: %s",
			     error ? error : "(no error reported)");
			bfree(error);
		}
	}

	return effect;
}

struct bt2446_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *method;
	gs_eparam_t *sdr_peak;
	gs_eparam_t *b_knee;
	gs_eparam_t *b_hlg_shaping;
	gs_eparam_t *a_rho_hdr;
	gs_eparam_t *a_inv_log_rho_hdr;
	gs_eparam_t *a_rho_sdr;
	gs_eparam_t *a_inv_rho_sdr_minus_one;
	gs_eparam_t *b_inv_lw_nits;
	gs_eparam_t *b_hlg_inv_gamma;
	gs_eparam_t *b_hlg_exponent;
	gs_eparam_t *b_hlg_ratio;
	gs_eparam_t *b_strength_over_slope;
	gs_eparam_t *b_slope_over_strength;
	gs_eparam_t *c_white_x;
	gs_eparam_t *c_white_y;
	gs_eparam_t *c_white_z;
	gs_eparam_t *c_peak_lightness;
	gs_eparam_t *inv_hdr_peak;
	gs_eparam_t *inv_sdr_peak;
	gs_eparam_t *c_matrix_in[3];
	gs_eparam_t *c_matrix_out[3];
	gs_eparam_t *c_k1;
	gs_eparam_t *c_k2;
	gs_eparam_t *c_k3;
	gs_eparam_t *c_k4;
	gs_eparam_t *c_knee;
	gs_eparam_t *c_hdr_reference_white;
	gs_eparam_t *c_white_shift;
	gs_eparam_t *input_scale;
	gs_eparam_t *output_scale;
	int selected_method;
	float hdr_peak_nits;
	float sdr_peak_nits;
	float b_knee_nits;
	float b_strength_nits;
	float b_slope_value;
	float b_hlg_shaping_value;
	float b_input_lw_nits;
	float a_rho_hdr_value;
	float a_inv_log_rho_hdr_value;
	float a_rho_sdr_value;
	float a_inv_rho_sdr_minus_one_value;
	float b_inv_lw_nits_value;
	float b_hlg_inv_gamma_value;
	float b_hlg_exponent_value;
	float b_hlg_ratio_value;
	float b_strength_over_slope_value;
	float b_slope_over_strength_value;
	float c_white_x_value;
	float c_white_y_value;
	float c_white_z_value;
	float c_peak_lightness_value;
	float inv_hdr_peak_nits_value;
	float inv_sdr_peak_nits_value;
	float c_alpha_value;
	float c_matrix_in_values[3][3];
	float c_matrix_out_values[3][3];
	float c_k1_value;
	float c_k2_value;
	float c_k3_value;
	float c_k4_value;
	float c_knee_sdr_nits;
	float c_knee_hdr_nits;
	float c_hdr_reference_white_nits;
	float c_sdr_reference_white_percent;
	float c_white_shift_value;
};

/* Colour-space matrices used by Method C. These are the same constants that used
 * to sit inline in the effect, kept here so the folded products are derived from
 * exactly the numbers that were being applied before. */
static const double matrix_709_to_2020[3][3] = {
	{0.6274038959, 0.3292830384, 0.0433130657},
	{0.0690972894, 0.9195403951, 0.0113623156},
	{0.0163914389, 0.0880133079, 0.8955952532},
};

static const double matrix_2020_to_709[3][3] = {
	{1.6604910021, -0.5876411388, -0.0728498633},
	{-0.1245504745, 1.1328998971, -0.0083494226},
	{-0.0181507634, -0.1005788980, 1.1187296614},
};

static const double matrix_2020_to_xyz[3][3] = {
	{0.6370, 0.1446, 0.1689},
	{0.2627, 0.6780, 0.0593},
	{0.0000, 0.0281, 1.0610},
};

static const double matrix_xyz_to_2020[3][3] = {
	{1.7167, -0.3557, -0.2534},
	{-0.6667, 1.6165, 0.0158},
	{0.0176, -0.0428, 0.9421},
};

static void matrix3_multiply(const double a[3][3], const double b[3][3], double out[3][3])
{
	for (size_t row = 0; row < 3; ++row) {
		for (size_t col = 0; col < 3; ++col) {
			out[row][col] = a[row][0] * b[0][col] + a[row][1] * b[1][col] +
					a[row][2] * b[2][col];
		}
	}
}

static double lab_f_for_parameters(double value)
{
	const double delta = 6.0 / 29.0;
	const double delta3 = delta * delta * delta;
	return value > delta3 ? pow(value, 1.0 / 3.0) : value / (3.0 * delta * delta) + 4.0 / 29.0;
}

/* The BT.2446 equations in the effect are written in terms of the configured
 * peaks, HLG input white, the knee shape and the Method C reference white.
 * Those values are fixed for an entire draw, so evaluating their pow(), log()
 * and division once per settings change avoids repeating them for every pixel.
 * Each assignment below keeps the shader's original clamp and is explained in
 * docs/hoisting-uniform-constants.md. */
static void build_derived_parameters(struct bt2446_filter *filter)
{
	const double hdr_peak = fmax(filter->hdr_peak_nits, 0.0001f);
	const double sdr_peak = fmax(filter->sdr_peak_nits, 0.0001f);
	const double rho_hdr = 1.0 + 32.0 * pow(hdr_peak / 10000.0, 1.0 / 2.4);
	const double rho_sdr = 1.0 + 32.0 * pow(sdr_peak / 10000.0, 1.0 / 2.4);
	const double lw_in = fmax(filter->b_input_lw_nits, 1.0f);
	const double hlg_gamma = 1.2 + 0.42 * (log(lw_in / 1000.0) * 0.4342944819);
	const double strength = fmax(filter->b_strength_nits, 0.0001f);
	const double slope = fmax(filter->b_slope_value, 0.0001f);
	const double reference_white = fmax(filter->c_hdr_reference_white_nits, 0.0001f);

	filter->a_rho_hdr_value = (float)rho_hdr;
	filter->a_inv_log_rho_hdr_value = (float)(1.0 / log(rho_hdr));
	filter->a_rho_sdr_value = (float)rho_sdr;
	filter->a_inv_rho_sdr_minus_one_value = (float)(1.0 / (rho_sdr - 1.0));

	filter->b_inv_lw_nits_value = (float)(1.0 / lw_in);
	filter->b_hlg_inv_gamma_value = (float)(1.0 / hlg_gamma);
	filter->b_hlg_exponent_value = (float)(1.03 - hlg_gamma);
	filter->b_hlg_ratio_value = (float)(291.0 / lw_in);
	filter->b_strength_over_slope_value = (float)(strength / slope);
	filter->b_slope_over_strength_value = (float)(slope / strength);

	filter->c_white_x_value = (float)(reference_white * 192.93 / 203.0);
	filter->c_white_y_value = (float)reference_white;
	filter->c_white_z_value = (float)(reference_white * 221.05 / 203.0);
	filter->c_peak_lightness_value =
		(float)fmax(116.0 * lab_f_for_parameters(hdr_peak / reference_white) - 16.0, 100.0001);

	filter->inv_hdr_peak_nits_value = (float)(1.0 / hdr_peak);
	filter->inv_sdr_peak_nits_value = (float)(1.0 / sdr_peak);
}

/* Method C runs Rec.709 -> Rec.2020 -> crosstalk -> XYZ, applies the tone curve,
 * then XYZ -> Rec.2020 -> inverse crosstalk -> Rec.709. Every step outside the
 * curve is a 3x3 matrix and the curve only rescales the XYZ triple, so each half
 * collapses into one matrix. The derivation is in
 * docs/method-c-matrix-folding.md. */
static void build_method_c_matrices(struct bt2446_filter *filter)
{
	const double alpha = filter->c_alpha_value;
	/* Same floor the shader used to apply inline when alpha reaches 1/3. */
	const double inverse_scale = 1.0 / fmax(1.0 - 3.0 * alpha, 0.01);
	double crosstalk[3][3];
	double crosstalk_inverse[3][3];
	double product[3][3];
	double result[3][3];

	for (size_t row = 0; row < 3; ++row) {
		for (size_t col = 0; col < 3; ++col) {
			const double identity = row == col ? 1.0 : 0.0;
			/* (1 - 3a)I + aJ */
			crosstalk[row][col] = (row == col ? 1.0 - 3.0 * alpha : 0.0) + alpha;
			/* (I - J/3)/(1 - 3a) + J/3, which is its exact inverse. */
			crosstalk_inverse[row][col] = (identity - 1.0 / 3.0) * inverse_scale + 1.0 / 3.0;
		}
	}

	matrix3_multiply(matrix_2020_to_xyz, crosstalk, product);
	matrix3_multiply(product, matrix_709_to_2020, result);
	for (size_t row = 0; row < 3; ++row) {
		for (size_t col = 0; col < 3; ++col)
			filter->c_matrix_in_values[row][col] = (float)result[row][col];
	}

	matrix3_multiply(matrix_2020_to_709, crosstalk_inverse, product);
	matrix3_multiply(product, matrix_xyz_to_2020, result);
	for (size_t row = 0; row < 3; ++row) {
		for (size_t col = 0; col < 3; ++col)
			filter->c_matrix_out_values[row][col] = (float)result[row][col];
	}
}

static float solve_c_k3(const struct bt2446_filter *filter, float knee_hdr)
{
	const float reference_hdr = fmaxf(filter->c_hdr_reference_white_nits, knee_hdr + 0.0001f);
	const float reference_sdr = 100.0f * powf(filter->c_sdr_reference_white_percent / 100.0f, 2.4f);
	const float target = fmaxf(reference_sdr - filter->c_k1_value * knee_hdr, 0.0001f);
	float low = 0.0001f;
	float high = 0.9999f;

	for (size_t i = 0; i < 48; ++i) {
		const float mid = 0.5f * (low + high);
		const float k2 = filter->c_k1_value * (1.0f - mid) * knee_hdr;
		const float k4 = filter->c_k1_value * knee_hdr - k2 * logf(1.0f - mid);
		const float mapped = k2 * logf(fmaxf(reference_hdr / knee_hdr - mid, 0.0001f)) + k4;
		if (mapped - filter->c_k1_value * knee_hdr > target)
			low = mid;
		else
			high = mid;
	}
	return 0.5f * (low + high);
}

static const char *bt2446_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BT2446.FilterName");
}

static void *bt2446_filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct bt2446_filter *filter = bzalloc(sizeof(*filter));
	filter->context = context;

	obs_enter_graphics();
	filter->effect = bt2446_load_effect();
	obs_leave_graphics();
	if (!filter->effect) {
		blog(LOG_ERROR, "[sorayuki-bt2446-tonemap] no usable effect, filter cannot be created");
		bfree(filter);
		return NULL;
	}

	filter->method = gs_effect_get_param_by_name(filter->effect, "method");
	filter->sdr_peak = gs_effect_get_param_by_name(filter->effect, "sdr_peak_nits");
	filter->b_knee = gs_effect_get_param_by_name(filter->effect, "b_knee_nits");
	filter->b_hlg_shaping = gs_effect_get_param_by_name(filter->effect, "b_hlg_shaping");
	filter->a_rho_hdr = gs_effect_get_param_by_name(filter->effect, "a_rho_hdr");
	filter->a_inv_log_rho_hdr = gs_effect_get_param_by_name(filter->effect, "a_inv_log_rho_hdr");
	filter->a_rho_sdr = gs_effect_get_param_by_name(filter->effect, "a_rho_sdr");
	filter->a_inv_rho_sdr_minus_one =
		gs_effect_get_param_by_name(filter->effect, "a_inv_rho_sdr_minus_one");
	filter->b_inv_lw_nits = gs_effect_get_param_by_name(filter->effect, "b_inv_lw_nits");
	filter->b_hlg_inv_gamma = gs_effect_get_param_by_name(filter->effect, "b_hlg_inv_gamma");
	filter->b_hlg_exponent = gs_effect_get_param_by_name(filter->effect, "b_hlg_exponent");
	filter->b_hlg_ratio = gs_effect_get_param_by_name(filter->effect, "b_hlg_ratio");
	filter->b_strength_over_slope =
		gs_effect_get_param_by_name(filter->effect, "b_strength_over_slope");
	filter->b_slope_over_strength =
		gs_effect_get_param_by_name(filter->effect, "b_slope_over_strength");
	filter->c_white_x = gs_effect_get_param_by_name(filter->effect, "c_white_x_nits");
	filter->c_white_y = gs_effect_get_param_by_name(filter->effect, "c_white_y_nits");
	filter->c_white_z = gs_effect_get_param_by_name(filter->effect, "c_white_z_nits");
	filter->c_peak_lightness = gs_effect_get_param_by_name(filter->effect, "c_peak_lightness");
	filter->inv_hdr_peak = gs_effect_get_param_by_name(filter->effect, "inv_hdr_peak_nits");
	filter->inv_sdr_peak = gs_effect_get_param_by_name(filter->effect, "inv_sdr_peak_nits");
	filter->c_matrix_in[0] = gs_effect_get_param_by_name(filter->effect, "c_matrix_in_0");
	filter->c_matrix_in[1] = gs_effect_get_param_by_name(filter->effect, "c_matrix_in_1");
	filter->c_matrix_in[2] = gs_effect_get_param_by_name(filter->effect, "c_matrix_in_2");
	filter->c_matrix_out[0] = gs_effect_get_param_by_name(filter->effect, "c_matrix_out_0");
	filter->c_matrix_out[1] = gs_effect_get_param_by_name(filter->effect, "c_matrix_out_1");
	filter->c_matrix_out[2] = gs_effect_get_param_by_name(filter->effect, "c_matrix_out_2");
	filter->c_k1 = gs_effect_get_param_by_name(filter->effect, "c_k1");
	filter->c_k2 = gs_effect_get_param_by_name(filter->effect, "c_k2");
	filter->c_k3 = gs_effect_get_param_by_name(filter->effect, "c_k3");
	filter->c_k4 = gs_effect_get_param_by_name(filter->effect, "c_k4");
	filter->c_knee = gs_effect_get_param_by_name(filter->effect, "c_knee_hdr_nits");
	filter->c_hdr_reference_white = gs_effect_get_param_by_name(filter->effect, "c_hdr_reference_white_nits");
	filter->c_white_shift = gs_effect_get_param_by_name(filter->effect, "c_white_shift");
	filter->input_scale = gs_effect_get_param_by_name(filter->effect, "input_scale");
	filter->output_scale = gs_effect_get_param_by_name(filter->effect, "output_scale");

	obs_source_update(context, settings);
	return filter;
}

static void bt2446_filter_destroy(void *data)
{
	struct bt2446_filter *filter = data;
	obs_enter_graphics();
	gs_effect_destroy(filter->effect);
	obs_leave_graphics();
	bfree(filter);
}

static void bt2446_filter_update(void *data, obs_data_t *settings)
{
	struct bt2446_filter *filter = data;
	filter->selected_method = (int)obs_data_get_int(settings, "method");
	filter->hdr_peak_nits = (float)obs_data_get_int(settings, "hdr_peak_nits");
	filter->sdr_peak_nits = (float)obs_data_get_int(settings, "sdr_peak_nits");
	filter->b_knee_nits = (float)obs_data_get_int(settings, "b_knee_nits");
	filter->b_strength_nits = (float)obs_data_get_int(settings, "b_strength_nits");
	filter->b_slope_value = (float)obs_data_get_double(settings, "b_slope");
	filter->b_hlg_shaping_value = obs_data_get_bool(settings, "b_hlg_shaping") ? 1.0f : 0.0f;
	filter->b_input_lw_nits = (float)obs_data_get_int(settings, "b_input_lw_nits");
	filter->c_alpha_value = (float)obs_data_get_double(settings, "c_alpha");
	filter->c_k1_value = (float)obs_data_get_double(settings, "c_k1");
	filter->c_knee_sdr_nits = (float)obs_data_get_double(settings, "c_knee_sdr_nits");
	filter->c_hdr_reference_white_nits = (float)obs_data_get_int(settings, "c_hdr_reference_white_nits");
	filter->c_sdr_reference_white_percent = (float)obs_data_get_int(settings, "c_sdr_reference_white_nits");
	filter->c_white_shift_value = (float)obs_data_get_double(settings, "c_white_shift");

	filter->c_knee_hdr_nits = fmaxf(filter->c_knee_sdr_nits / fmaxf(filter->c_k1_value, 0.0001f), 0.0001f);
	filter->c_k3_value = solve_c_k3(filter, filter->c_knee_hdr_nits);
	filter->c_k2_value = filter->c_k1_value * (1.0f - filter->c_k3_value) * filter->c_knee_hdr_nits;
	filter->c_k4_value = filter->c_k1_value * filter->c_knee_hdr_nits -
			     filter->c_k2_value * logf(1.0f - filter->c_k3_value);
	build_derived_parameters(filter);
	build_method_c_matrices(filter);
}

static bool method_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	const int method = (int)obs_data_get_int(settings, "method");
	const bool show_b = method == METHOD_B;
	const bool show_c = method == METHOD_C;
	obs_property_set_visible(obs_properties_get(props, "hdr_peak_nits"), !show_b);
	obs_property_set_visible(obs_properties_get(props, "sdr_peak_nits"), true);
	obs_property_set_visible(obs_properties_get(props, "b_knee_nits"), show_b);
	obs_property_set_visible(obs_properties_get(props, "b_strength_nits"), show_b);
	obs_property_set_visible(obs_properties_get(props, "b_slope"), show_b);
	obs_property_set_visible(obs_properties_get(props, "b_hlg_shaping"), show_b);
	obs_property_set_visible(obs_properties_get(props, "b_input_lw_nits"),
				 show_b && obs_data_get_bool(settings, "b_hlg_shaping"));
	obs_property_set_visible(obs_properties_get(props, "c_alpha"), show_c);
	obs_property_set_visible(obs_properties_get(props, "c_k1"), show_c);
	obs_property_set_visible(obs_properties_get(props, "c_knee_sdr_nits"), show_c);
	obs_property_set_visible(obs_properties_get(props, "c_hdr_reference_white_nits"), show_c);
	obs_property_set_visible(obs_properties_get(props, "c_sdr_reference_white_nits"), show_c);
	obs_property_set_visible(obs_properties_get(props, "c_white_shift"), show_c);
	UNUSED_PARAMETER(property);
	return true;
}

static bool hlg_shaping_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	obs_property_set_visible(obs_properties_get(props, "b_input_lw_nits"),
				 obs_data_get_bool(settings, "b_hlg_shaping"));
	UNUSED_PARAMETER(property);
	return true;
}

static obs_properties_t *bt2446_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "method", obs_module_text("BT2446.Method"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("BT2446.MethodA"), METHOD_A);
	obs_property_list_add_int(p, obs_module_text("BT2446.MethodB"), METHOD_B);
	obs_property_list_add_int(p, obs_module_text("BT2446.MethodC"), METHOD_C);
	obs_property_set_long_description(p, obs_module_text("BT2446.Method.Desc"));
	obs_property_set_modified_callback(p, method_changed);

	p = obs_properties_add_int(props, "hdr_peak_nits", obs_module_text("BT2446.HdrPeak"), 100, 10000, 10);
	obs_property_set_long_description(p, obs_module_text("BT2446.HdrPeak.Desc"));
	p = obs_properties_add_int(props, "sdr_peak_nits", obs_module_text("BT2446.SdrPeak"), 40, 300, 1);
	obs_property_set_long_description(p, obs_module_text("BT2446.SdrPeak.Desc"));

	p = obs_properties_add_int(props, "b_knee_nits", obs_module_text("BT2446.BKnee"), 1, 500, 1);
	obs_property_set_long_description(p, obs_module_text("BT2446.BKnee.Desc"));
	p = obs_properties_add_int(props, "b_strength_nits", obs_module_text("BT2446.BStrength"), 1, 200, 1);
	obs_property_set_long_description(p, obs_module_text("BT2446.BStrength.Desc"));
	p = obs_properties_add_float_slider(props, "b_slope", obs_module_text("BT2446.BSlope"), 0.5, 2.0, 0.01);
	obs_property_set_long_description(p, obs_module_text("BT2446.BSlope.Desc"));
	p = obs_properties_add_bool(props, "b_hlg_shaping", obs_module_text("BT2446.BHlgShaping"));
	obs_property_set_long_description(p, obs_module_text("BT2446.BHlgShaping.Desc"));
	obs_property_set_modified_callback(p, hlg_shaping_changed);
	p = obs_properties_add_int(props, "b_input_lw_nits", obs_module_text("BT2446.BInputLw"), 100, 10000, 10);
	obs_property_set_long_description(p, obs_module_text("BT2446.BInputLw.Desc"));

	p = obs_properties_add_float_slider(props, "c_alpha", obs_module_text("BT2446.CAlpha"), 0.0, 0.33, 0.01);
	obs_property_set_long_description(p, obs_module_text("BT2446.CAlpha.Desc"));
	p = obs_properties_add_float_slider(props, "c_k1", obs_module_text("BT2446.CK1"), 0.1, 1.5, 0.0001);
	obs_property_set_long_description(p, obs_module_text("BT2446.CK1.Desc"));
	p = obs_properties_add_float(props, "c_knee_sdr_nits", obs_module_text("BT2446.CKnee"), 20.0, 90.0, 0.1);
	obs_property_set_long_description(p, obs_module_text("BT2446.CKnee.Desc"));
	p = obs_properties_add_int(props, "c_hdr_reference_white_nits", obs_module_text("BT2446.CHdrReferenceWhite"),
				   50, 500, 1);
	obs_property_set_long_description(p, obs_module_text("BT2446.CHdrReferenceWhite.Desc"));
	p = obs_properties_add_int(props, "c_sdr_reference_white_nits", obs_module_text("BT2446.CSdrReferenceWhite"),
				   20, 100, 1);
	obs_property_set_long_description(p, obs_module_text("BT2446.CSdrReferenceWhite.Desc"));
	p = obs_properties_add_float_slider(props, "c_white_shift", obs_module_text("BT2446.CWhiteShift"), 0.0,
					    1.0, 0.01);
	obs_property_set_long_description(p, obs_module_text("BT2446.CWhiteShift.Desc"));

	obs_data_t *settings = obs_source_get_settings(((struct bt2446_filter *)data)->context);
	method_changed(props, NULL, settings);
	obs_data_release(settings);
	return props;
}

static void bt2446_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "method", METHOD_A);
	obs_data_set_default_int(settings, "hdr_peak_nits", 1000);
	obs_data_set_default_int(settings, "sdr_peak_nits", 100);
	obs_data_set_default_int(settings, "b_knee_nits", 55);
	obs_data_set_default_int(settings, "b_strength_nits", 16);
	obs_data_set_default_double(settings, "b_slope", 1.0);
	obs_data_set_default_bool(settings, "b_hlg_shaping", true);
	obs_data_set_default_int(settings, "b_input_lw_nits", 1000);
	obs_data_set_default_double(settings, "c_alpha", 0.1);
	obs_data_set_default_double(settings, "c_k1", 0.83802);
	obs_data_set_default_double(settings, "c_knee_sdr_nits", 58.5);
	obs_data_set_default_int(settings, "c_hdr_reference_white_nits", 203);
	obs_data_set_default_int(settings, "c_sdr_reference_white_nits", 96);
	obs_data_set_default_double(settings, "c_white_shift", 0.0);
}

/* Nits represented by 1.0 in the space of the render target the filter draws
 * into. The output is normalised with this value so that the pixels handed to
 * the compositor follow the convention of the target space instead of always
 * being scRGB-normalised. */
static float target_white_nits(enum gs_color_space space)
{
	switch (space) {
	case GS_CS_709_SCRGB:
		return 80.0f;
	case GS_CS_709_EXTENDED:
		return obs_get_video_sdr_white_level();
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
	default:
		/* SDR reference white (BT.1886) */
		return 100.0f;
	}
}

static void bt2446_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct bt2446_filter *filter = data;
	/* Space of the render target the result will end up in. Must be sampled
	 * before the filter's own texrender becomes current. */
	const enum gs_color_space output_space = gs_get_color_space();
	/* GS_CS_709_SCRGB first: Windows Graphics Capture and game capture hand
	 * out native scRGB (1.0 = 80 cd/m2) when it is offered, which removes the
	 * ambiguity between scRGB and OBS' SDR-white-relative extended range. */
	const enum gs_color_space preferred_spaces[] = {GS_CS_709_SCRGB, GS_CS_709_EXTENDED, GS_CS_SRGB_16F,
							GS_CS_SRGB};
	const enum gs_color_space source_space = obs_source_get_color_space(
		obs_filter_get_target(filter->context), OBS_COUNTOF(preferred_spaces), preferred_spaces);
	if (source_space != GS_CS_709_EXTENDED && source_space != GS_CS_709_SCRGB) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const enum gs_color_format format = gs_get_format_from_space(source_space);
	if (!obs_source_process_filter_begin_with_color_space(filter->context, format, source_space,
							     OBS_NO_DIRECT_RENDERING))
		return;

	gs_effect_set_float(filter->method, (float)filter->selected_method);
	gs_effect_set_float(filter->sdr_peak, filter->sdr_peak_nits);
	gs_effect_set_float(filter->b_knee, filter->b_knee_nits);
	gs_effect_set_float(filter->b_hlg_shaping, filter->b_hlg_shaping_value);
	gs_effect_set_float(filter->a_rho_hdr, filter->a_rho_hdr_value);
	gs_effect_set_float(filter->a_inv_log_rho_hdr, filter->a_inv_log_rho_hdr_value);
	gs_effect_set_float(filter->a_rho_sdr, filter->a_rho_sdr_value);
	gs_effect_set_float(filter->a_inv_rho_sdr_minus_one,
			    filter->a_inv_rho_sdr_minus_one_value);
	gs_effect_set_float(filter->b_inv_lw_nits, filter->b_inv_lw_nits_value);
	gs_effect_set_float(filter->b_hlg_inv_gamma, filter->b_hlg_inv_gamma_value);
	gs_effect_set_float(filter->b_hlg_exponent, filter->b_hlg_exponent_value);
	gs_effect_set_float(filter->b_hlg_ratio, filter->b_hlg_ratio_value);
	gs_effect_set_float(filter->b_strength_over_slope, filter->b_strength_over_slope_value);
	gs_effect_set_float(filter->b_slope_over_strength, filter->b_slope_over_strength_value);
	gs_effect_set_float(filter->c_white_x, filter->c_white_x_value);
	gs_effect_set_float(filter->c_white_y, filter->c_white_y_value);
	gs_effect_set_float(filter->c_white_z, filter->c_white_z_value);
	gs_effect_set_float(filter->c_peak_lightness, filter->c_peak_lightness_value);
	gs_effect_set_float(filter->inv_hdr_peak, filter->inv_hdr_peak_nits_value);
	gs_effect_set_float(filter->inv_sdr_peak, filter->inv_sdr_peak_nits_value);
	for (size_t i = 0; i < 3; ++i) {
		gs_effect_set_val(filter->c_matrix_in[i], filter->c_matrix_in_values[i], sizeof(float) * 3);
		gs_effect_set_val(filter->c_matrix_out[i], filter->c_matrix_out_values[i], sizeof(float) * 3);
	}
	gs_effect_set_float(filter->c_k1, filter->c_k1_value);
	gs_effect_set_float(filter->c_k2, filter->c_k2_value);
	gs_effect_set_float(filter->c_k3, filter->c_k3_value);
	gs_effect_set_float(filter->c_k4, filter->c_k4_value);
	gs_effect_set_float(filter->c_knee, filter->c_knee_hdr_nits);
	gs_effect_set_float(filter->c_hdr_reference_white, filter->c_hdr_reference_white_nits);
	gs_effect_set_float(filter->c_white_shift, filter->c_white_shift_value);
	gs_effect_set_float(filter->input_scale,
			    source_space == GS_CS_709_SCRGB ? 80.0f : obs_get_video_sdr_white_level());
	/* The result is written as if it were a regular SDR source in the space of
	 * the current render target, so the compositor has nothing left to map. */
	gs_effect_set_float(filter->output_scale, 1.0f / fmaxf(target_white_nits(output_space), 0.0001f));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(filter->context, filter->effect, 0, 0, "Draw");
	gs_blend_state_pop();
}

static enum gs_color_space bt2446_filter_get_color_space(void *data, size_t count,
							 const enum gs_color_space *preferred_spaces)
{
	struct bt2446_filter *filter = data;
	const enum gs_color_space source_space =
		obs_source_get_color_space(obs_filter_get_target(filter->context), 0, NULL);

	/* Only HDR inputs are converted; everything else is passed through
	 * untouched, so keep reporting the source's own space in that case. */
	if (source_space != GS_CS_709_EXTENDED && source_space != GS_CS_709_SCRGB)
		return source_space;

	/* The filter emits SDR, therefore the caller must composite it as an SDR
	 * source. Honour the requested spaces so libobs does not have to convert
	 * the result again. */
	for (size_t i = 0; i < count; ++i) {
		if (preferred_spaces[i] == GS_CS_SRGB)
			return GS_CS_SRGB;
	}
	for (size_t i = 0; i < count; ++i) {
		if (preferred_spaces[i] == GS_CS_SRGB_16F)
			return GS_CS_SRGB_16F;
	}

	return GS_CS_SRGB;
}

struct obs_source_info bt2446_filter = {
	.id = "bt2446_hdr_to_sdr_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = bt2446_filter_get_name,
	.create = bt2446_filter_create,
	.destroy = bt2446_filter_destroy,
	.update = bt2446_filter_update,
	.get_properties = bt2446_filter_properties,
	.get_defaults = bt2446_filter_defaults,
	.video_render = bt2446_filter_render,
	.video_get_color_space = bt2446_filter_get_color_space,
};
