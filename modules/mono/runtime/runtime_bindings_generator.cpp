#include "runtime_bindings_generator.h"
#include "../utils/naming_utils.h"
#include "../utils/string_utils.h"

using TypeInterface = RuntimeBindingsGenerator::TypeInterface;

void TypeInterface::postsetup_enum_type(TypeInterface &r_enum_itype) {
	// C interface for enums is the same as that of 'uint32_t'. Remember to apply
	// any of the changes done here to the 'uint32_t' type interface as well.

	r_enum_itype.cs_type = r_enum_itype.proxy_name;
	r_enum_itype.cs_in_expr = "(int)%0";
	r_enum_itype.cs_out = "%5return (%2)%0(%1);";

	{
		// The expected types for parameters and return value in ptrcall are 'int64_t' or 'uint64_t'.
		r_enum_itype.c_in = "%5%0 %1_in = %1;\n";
		r_enum_itype.c_out = "%5return (%0)(%1);\n";
		r_enum_itype.c_type = "long";
		r_enum_itype.c_arg_in = "&%s_in";
	}
	r_enum_itype.c_type_in = "int";
	r_enum_itype.c_type_out = r_enum_itype.c_type_in;
	r_enum_itype.class_doc = &EditorHelp::get_doc_data()->class_list[r_enum_itype.proxy_name];
}

RuntimeBindingsGenerator::RuntimeBindingsGenerator() {
}

void RuntimeBindingsGenerator::_log(const char *p_format, ...) {
	if (log_print_enabled) {
		va_list list;

		va_start(list, p_format);
		OS::get_singleton()->print("%s", str_format(p_format, list).utf8().get_data());
		va_end(list);
	}
}

Error RuntimeBindingsGenerator::_save_file(const String &p_path, const StringBuilder &p_content) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_FILE_CANT_WRITE, "Cannot open file: '" + p_path + "'.");

	file->store_string(p_content.as_string());

	return OK;
}
