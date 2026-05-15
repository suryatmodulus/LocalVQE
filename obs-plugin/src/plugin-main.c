#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-localvqe", "en-US")

extern struct obs_source_info localvqe_filter_info;

bool obs_module_load(void)
{
	obs_register_source(&localvqe_filter_info);
	return true;
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "LocalVQE: neural AEC, noise suppression, and dereverberation";
}
