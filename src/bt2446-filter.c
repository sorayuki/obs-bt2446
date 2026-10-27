#include <math.h>
#include <obs-module.h>

#define METHOD_A 0
#define METHOD_B 1
#define METHOD_C 2

struct bt2446_filter {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *method;
	gs_eparam_t *hdr_peak;
	gs_eparam_t *sdr_peak;
	gs_eparam_t *b_knee;
	gs_eparam_t *b_strength;
	gs_eparam_t *b_slope_param;
	gs_eparam_t *b_hlg_shaping;
	gs_eparam_t *b_input_lw;
	gs_eparam_t *c_alpha;
	gs_eparam_t *c_k1;
	gs_eparam_t *c_k2;
	gs_eparam_t *c_k3;
	gs_eparam_t *c_k4;
	gs_eparam_t *c_knee;
	gs_eparam_t *c_hdr_reference_white;
	gs_eparam_t *c_white_shift;
	gs_eparam_t *input_scale;
	int selected_method;
	float hdr_peak_nits;
	float sdr_peak_nits;
	float b_knee_nits;
	float b_strength_nits;
	float b_slope_value;
	float b_hlg_shaping_value;
	float b_input_lw_nits;
	float c_alpha_value;
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
	char *effect_path = obs_module_file("bt2446.effect");

	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();
	bfree(effect_path);
	if (!filter->effect) {
		bfree(filter);
		return NULL;
	}

	filter->method = gs_effect_get_param_by_name(filter->effect, "method");
	filter->hdr_peak = gs_effect_get_param_by_name(filter->effect, "hdr_peak_nits");
	filter->sdr_peak = gs_effect_get_param_by_name(filter->effect, "sdr_peak_nits");
	filter->b_knee = gs_effect_get_param_by_name(filter->effect, "b_knee_nits");
	filter->b_strength = gs_effect_get_param_by_name(filter->effect, "b_strength_nits");
	filter->b_slope_param = gs_effect_get_param_by_name(filter->effect, "b_slope");
	filter->b_hlg_shaping = gs_effect_get_param_by_name(filter->effect, "b_hlg_shaping");
	filter->b_input_lw = gs_effect_get_param_by_name(filter->effect, "b_input_lw_nits");
	filter->c_alpha = gs_effect_get_param_by_name(filter->effect, "c_alpha");
	filter->c_k1 = gs_effect_get_param_by_name(filter->effect, "c_k1");
	filter->c_k2 = gs_effect_get_param_by_name(filter->effect, "c_k2");
	filter->c_k3 = gs_effect_get_param_by_name(filter->effect, "c_k3");
	filter->c_k4 = gs_effect_get_param_by_name(filter->effect, "c_k4");
	filter->c_knee = gs_effect_get_param_by_name(filter->effect, "c_knee_hdr_nits");
	filter->c_hdr_reference_white = gs_effect_get_param_by_name(filter->effect, "c_hdr_reference_white_nits");
	filter->c_white_shift = gs_effect_get_param_by_name(filter->effect, "c_white_shift");
	filter->input_scale = gs_effect_get_param_by_name(filter->effect, "input_scale");

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
	obs_property_t *method = obs_properties_add_list(props, "method", obs_module_text("BT2446.Method"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(method, obs_module_text("BT2446.MethodA"), METHOD_A);
	obs_property_list_add_int(method, obs_module_text("BT2446.MethodB"), METHOD_B);
	obs_property_list_add_int(method, obs_module_text("BT2446.MethodC"), METHOD_C);
	obs_property_set_modified_callback(method, method_changed);

	obs_properties_add_int(props, "hdr_peak_nits", obs_module_text("BT2446.HdrPeak"), 100, 10000, 10);
	obs_properties_add_int(props, "sdr_peak_nits", obs_module_text("BT2446.SdrPeak"), 40, 300, 1);
	obs_properties_add_int(props, "b_knee_nits", obs_module_text("BT2446.BKnee"), 1, 500, 1);
	obs_properties_add_int(props, "b_strength_nits", obs_module_text("BT2446.BStrength"), 1, 200, 1);
	obs_properties_add_float_slider(props, "b_slope", obs_module_text("BT2446.BSlope"), 0.5, 2.0, 0.01);
	obs_property_t *hlg_shaping =
		obs_properties_add_bool(props, "b_hlg_shaping", obs_module_text("BT2446.BHlgShaping"));
	obs_property_set_modified_callback(hlg_shaping, hlg_shaping_changed);
	obs_properties_add_int(props, "b_input_lw_nits", obs_module_text("BT2446.BInputLw"), 100, 10000, 10);
	obs_properties_add_float_slider(props, "c_alpha", obs_module_text("BT2446.CAlpha"), 0.0, 0.33, 0.01);
	obs_properties_add_float_slider(props, "c_k1", obs_module_text("BT2446.CK1"), 0.1, 1.5, 0.0001);
	obs_properties_add_float(props, "c_knee_sdr_nits", obs_module_text("BT2446.CKnee"), 20.0, 90.0, 0.1);
	obs_properties_add_int(props, "c_hdr_reference_white_nits", obs_module_text("BT2446.CHdrReferenceWhite"), 50,
			       500, 1);
	obs_properties_add_int(props, "c_sdr_reference_white_nits", obs_module_text("BT2446.CSdrReferenceWhite"), 20,
			       100, 1);
	obs_properties_add_float_slider(props, "c_white_shift", obs_module_text("BT2446.CWhiteShift"), 0.0, 1.0,
					0.01);

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

static void bt2446_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct bt2446_filter *filter = data;
	const enum gs_color_space preferred_spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
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
	gs_effect_set_float(filter->hdr_peak, filter->hdr_peak_nits);
	gs_effect_set_float(filter->sdr_peak, filter->sdr_peak_nits);
	gs_effect_set_float(filter->b_knee, filter->b_knee_nits);
	gs_effect_set_float(filter->b_strength, filter->b_strength_nits);
	gs_effect_set_float(filter->b_slope_param, filter->b_slope_value);
	gs_effect_set_float(filter->b_hlg_shaping, filter->b_hlg_shaping_value);
	gs_effect_set_float(filter->b_input_lw, filter->b_input_lw_nits);
	gs_effect_set_float(filter->c_alpha, filter->c_alpha_value);
	gs_effect_set_float(filter->c_k1, filter->c_k1_value);
	gs_effect_set_float(filter->c_k2, filter->c_k2_value);
	gs_effect_set_float(filter->c_k3, filter->c_k3_value);
	gs_effect_set_float(filter->c_k4, filter->c_k4_value);
	gs_effect_set_float(filter->c_knee, filter->c_knee_hdr_nits);
	gs_effect_set_float(filter->c_hdr_reference_white, filter->c_hdr_reference_white_nits);
	gs_effect_set_float(filter->c_white_shift, filter->c_white_shift_value);
	gs_effect_set_float(filter->input_scale, source_space == GS_CS_709_SCRGB ? 80.0f : obs_get_video_sdr_white_level());

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(filter->context, filter->effect, 0, 0, "Draw");
	gs_blend_state_pop();
}

static enum gs_color_space bt2446_filter_get_color_space(void *data, size_t count,
							 const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	return GS_CS_SRGB_16F;
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
