/**************************************************************************/
/*  bindings_generator.h                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef DEBUG_ENABLED

#include "core/doc_data.h"
#include "core/object/class_db.h"
#include "core/string/string_builder.h"
#include "core/string/ustring.h"
#include "core/typedefs.h"
#include "editor/doc/doc_tools.h"
#include "editor/doc/editor_help.h"
#include "modules/mono/runtime/runtime_bindings_generator.h"

class BindingsGenerator {
	bool initialized = false;

	RuntimeBindingsGenerator generator;

	HashMap<StringName, RuntimeBindingsGenerator::TypeInterface> obj_types;
	HashMap<StringName, RuntimeBindingsGenerator::TypeInterface> builtin_types;
	HashMap<StringName, RuntimeBindingsGenerator::TypeInterface> enum_types;
	List<RuntimeBindingsGenerator::EnumInterface> global_enums;
	List<RuntimeBindingsGenerator::ConstantInterface> global_constants;

	List<RuntimeBindingsGenerator::InternalCall> method_icalls;
	/// Stores the unique internal calls from [method_icalls] that are assigned to each method.
	HashMap<const RuntimeBindingsGenerator::MethodInterface *, const RuntimeBindingsGenerator::InternalCall *> method_icalls_map;

	//HashMap<StringName, List<StringName>> blacklisted_methods;
	//HashSet<StringName> compat_singletons;

	void _initialize_blacklisted_methods();
	void _initialize_compat_singletons();

	inline String get_arg_unique_sig(const RuntimeBindingsGenerator::TypeInterface &p_type) {
		// For parameters, we treat reference and non-reference derived types the same.
		if (p_type.is_object_type) {
			return "Obj";
		} else if (p_type.is_enum) {
			return "int";
		} else if (p_type.cname == generator.name_cache.type_Array_generic) {
			return "Array";
		} else if (p_type.cname == generator.name_cache.type_Dictionary_generic) {
			return "Dictionary";
		}

		return p_type.name;
	}

	inline String get_ret_unique_sig(const RuntimeBindingsGenerator::TypeInterface *p_type) {
		// Reference derived return types are treated differently.
		if (p_type->is_ref_counted) {
			return "Ref";
		} else if (p_type->is_object_type) {
			return "Obj";
		} else if (p_type->is_enum) {
			return "int";
		} else if (p_type->cname == generator.name_cache.type_Array_generic) {
			return "Array";
		} else if (p_type->cname == generator.name_cache.type_Dictionary_generic) {
			return "Dictionary";
		}

		return p_type->name;
	}

	Error _populate_method_icalls_table(const RuntimeBindingsGenerator::TypeInterface &p_itype);

	bool _populate_object_type_interfaces();
	void _populate_builtin_type_interfaces();

	void _populate_global_constants();

	Error _generate_cs_property(const RuntimeBindingsGenerator::TypeInterface &p_itype, const RuntimeBindingsGenerator::PropertyInterface &p_iprop, StringBuilder &p_output);
	Error _generate_cs_method(const RuntimeBindingsGenerator::TypeInterface &p_itype, const RuntimeBindingsGenerator::MethodInterface &p_imethod, int &p_method_bind_count, StringBuilder &p_output, bool p_use_span);
	Error _generate_cs_signal(const RuntimeBindingsGenerator::TypeInterface &p_itype, const RuntimeBindingsGenerator::SignalInterface &p_isignal, StringBuilder &p_output);

	Error _generate_cs_native_calls(const RuntimeBindingsGenerator::InternalCall &p_icall, StringBuilder &r_output);

	void _generate_array_extensions(StringBuilder &p_output);
	void _generate_global_constants(StringBuilder &p_output);

	void _initialize();

public:
	Error generate_cs_core_project(const String &p_proj_dir);
	Error generate_cs_editor_project(const String &p_proj_dir);
	Error generate_cs_api(const String &p_output_dir);

	_FORCE_INLINE_ bool is_log_print_enabled() { return generator.log_print_enabled; }
	_FORCE_INLINE_ void set_log_print_enabled(bool p_enabled) { generator.log_print_enabled = p_enabled; }

	_FORCE_INLINE_ bool is_initialized() { return initialized; }

	static void handle_cmdline_args(const List<String> &p_cmdline_args);

	BindingsGenerator() {
		_initialize();
	}
};

#endif // DEBUG_ENABLED
