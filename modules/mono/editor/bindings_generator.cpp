/**************************************************************************/
/*  bindings_generator.cpp                                                */
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

#include "bindings_generator.h"

#ifdef DEBUG_ENABLED

#include "../godotsharp_defs.h"
#include "../utils/naming_utils.h"
#include "../utils/path_utils.h"
#include "../utils/string_utils.h"

#include "core/config/engine.h"
#include "core/core_constants.h"
#include "core/io/compression.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "main/main.h"

#include "../bindings_generator_defs.h"

using TypeInterface = RuntimeBindingsGenerator::TypeInterface;
using PropertyInterface = RuntimeBindingsGenerator::PropertyInterface;
using MethodInterface = RuntimeBindingsGenerator::MethodInterface;
using SignalInterface = RuntimeBindingsGenerator::SignalInterface;
using EnumInterface = RuntimeBindingsGenerator::EnumInterface;
using ConstantInterface = RuntimeBindingsGenerator::ConstantInterface;
using ArgumentInterface = RuntimeBindingsGenerator::ArgumentInterface;
using TypeReference = RuntimeBindingsGenerator::TypeReference;
using InternalCall = RuntimeBindingsGenerator::InternalCall;

Error BindingsGenerator::_populate_method_icalls_table(const TypeInterface &p_itype) {
	for (const MethodInterface &imethod : p_itype.methods) {
		if (imethod.is_virtual) {
			continue;
		}

		const TypeInterface *return_type = generator._get_type_or_null(imethod.return_type, builtin_types,
				obj_types, enum_types);
		ERR_FAIL_NULL_V_MSG(return_type, ERR_BUG, "Return type '" + imethod.return_type.cname + "' was not found.");

		String im_unique_sig = get_ret_unique_sig(return_type) + ",CallMethodBind";

		if (!imethod.is_static) {
			im_unique_sig += ",CallInstance";
		}

		// Get arguments information
		for (const ArgumentInterface &iarg : imethod.arguments) {
			const TypeInterface *arg_type = generator._get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);
			ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

			im_unique_sig += ",";
			im_unique_sig += get_arg_unique_sig(*arg_type);
		}

		// godot_icall_{argc}_{icallcount}
		String icall_method = ICALL_PREFIX;
		icall_method += itos(imethod.arguments.size());
		icall_method += "_";
		icall_method += itos(method_icalls.size());

		InternalCall im_icall = InternalCall(p_itype.api_type, icall_method, im_unique_sig);

		im_icall.is_vararg = imethod.is_vararg;
		im_icall.is_static = imethod.is_static;
		im_icall.return_type = imethod.return_type;

		for (const List<ArgumentInterface>::Element *F = imethod.arguments.front(); F; F = F->next()) {
			im_icall.argument_types.push_back(F->get().type);
		}

		List<InternalCall>::Element *match = method_icalls.find(im_icall);

		if (match) {
			if (p_itype.api_type != ClassDB::API_EDITOR) {
				match->get().editor_only = false;
			}
			method_icalls_map.insert(&imethod, &match->get());
		} else {
			List<InternalCall>::Element *added = method_icalls.push_back(im_icall);
			method_icalls_map.insert(&imethod, &added->get());
		}
	}

	return OK;
}

void BindingsGenerator::_generate_array_extensions(StringBuilder &p_output) {
	p_output.append("namespace " BINDINGS_NAMESPACE ";\n\n");
	p_output.append("using System;\n\n");
	// The class where we put the extensions doesn't matter, so just use "GD".
	p_output.append("public static partial class " BINDINGS_GLOBAL_SCOPE_CLASS "\n{");

#define ARRAY_IS_EMPTY(m_type)                                                                          \
	p_output.append("\n" INDENT1 "/// <summary>\n");                                                    \
	p_output.append(INDENT1 "/// Returns true if this " #m_type " array is empty or doesn't exist.\n"); \
	p_output.append(INDENT1 "/// </summary>\n");                                                        \
	p_output.append(INDENT1 "/// <param name=\"instance\">The " #m_type " array check.</param>\n");     \
	p_output.append(INDENT1 "/// <returns>Whether or not the array is empty.</returns>\n");             \
	p_output.append(INDENT1 "public static bool IsEmpty(this " #m_type "[] instance)\n");               \
	p_output.append(OPEN_BLOCK_L1);                                                                     \
	p_output.append(INDENT2 "return instance == null || instance.Length == 0;\n");                      \
	p_output.append(INDENT1 CLOSE_BLOCK);

#define ARRAY_JOIN(m_type)                                                                                          \
	p_output.append("\n" INDENT1 "/// <summary>\n");                                                                \
	p_output.append(INDENT1 "/// Converts this " #m_type " array to a string delimited by the given string.\n");    \
	p_output.append(INDENT1 "/// </summary>\n");                                                                    \
	p_output.append(INDENT1 "/// <param name=\"instance\">The " #m_type " array to convert.</param>\n");            \
	p_output.append(INDENT1 "/// <param name=\"delimiter\">The delimiter to use between items.</param>\n");         \
	p_output.append(INDENT1 "/// <returns>A single string with all items.</returns>\n");                            \
	p_output.append(INDENT1 "public static string Join(this " #m_type "[] instance, string delimiter = \", \")\n"); \
	p_output.append(OPEN_BLOCK_L1);                                                                                 \
	p_output.append(INDENT2 "return String.Join(delimiter, instance);\n");                                          \
	p_output.append(INDENT1 CLOSE_BLOCK);

#define ARRAY_STRINGIFY(m_type)                                                                          \
	p_output.append("\n" INDENT1 "/// <summary>\n");                                                     \
	p_output.append(INDENT1 "/// Converts this " #m_type " array to a string with brackets.\n");         \
	p_output.append(INDENT1 "/// </summary>\n");                                                         \
	p_output.append(INDENT1 "/// <param name=\"instance\">The " #m_type " array to convert.</param>\n"); \
	p_output.append(INDENT1 "/// <returns>A single string with all items.</returns>\n");                 \
	p_output.append(INDENT1 "public static string Stringify(this " #m_type "[] instance)\n");            \
	p_output.append(OPEN_BLOCK_L1);                                                                      \
	p_output.append(INDENT2 "return \"[\" + instance.Join() + \"]\";\n");                                \
	p_output.append(INDENT1 CLOSE_BLOCK);

#define ARRAY_ALL(m_type)  \
	ARRAY_IS_EMPTY(m_type) \
	ARRAY_JOIN(m_type)     \
	ARRAY_STRINGIFY(m_type)

	ARRAY_ALL(byte);
	ARRAY_ALL(int);
	ARRAY_ALL(long);
	ARRAY_ALL(float);
	ARRAY_ALL(double);
	ARRAY_ALL(string);
	ARRAY_ALL(Color);
	ARRAY_ALL(Vector2);
	ARRAY_ALL(Vector2I);
	ARRAY_ALL(Vector3);
	ARRAY_ALL(Vector3I);
	ARRAY_ALL(Vector4);
	ARRAY_ALL(Vector4I);

#undef ARRAY_ALL
#undef ARRAY_IS_EMPTY
#undef ARRAY_JOIN
#undef ARRAY_STRINGIFY

	p_output.append(CLOSE_BLOCK); // End of GD class.
}

void BindingsGenerator::_generate_global_constants(StringBuilder &p_output) {
	// Constants (in partial GD class)

	p_output.append("namespace " BINDINGS_NAMESPACE ";\n\n");

	p_output.append("public static partial class " BINDINGS_GLOBAL_SCOPE_CLASS "\n" OPEN_BLOCK);

	for (const ConstantInterface &iconstant : global_constants) {
		if (iconstant.const_doc && iconstant.const_doc->description.size()) {
			String xml_summary = generator.bbcode_to_xml(fix_doc_description(iconstant.const_doc->description), nullptr,
					builtin_types, obj_types, enum_types, global_constants, global_enums);
			Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

			if (summary_lines.size()) {
				p_output.append(MEMBER_BEGIN "/// <summary>\n");

				for (int i = 0; i < summary_lines.size(); i++) {
					p_output.append(INDENT1 "/// ");
					p_output.append(summary_lines[i]);
					p_output.append("\n");
				}

				p_output.append(INDENT1 "/// </summary>");
			}
		}

		p_output.append(MEMBER_BEGIN "public const long ");
		p_output.append(iconstant.proxy_name);
		p_output.append(" = ");
		p_output.append(itos(iconstant.value));
		p_output.append(";");
	}

	if (!global_constants.is_empty()) {
		p_output.append("\n");
	}

	p_output.append(CLOSE_BLOCK); // end of GD class

	// Enums

	for (const EnumInterface &ienum : global_enums) {
		CRASH_COND(ienum.constants.is_empty());

		String enum_proxy_name = ienum.proxy_name;

		bool enum_in_static_class = false;

		if (enum_proxy_name.find_char('.') > 0) {
			enum_in_static_class = true;
			String enum_class_name = enum_proxy_name.get_slicec('.', 0);
			enum_proxy_name = enum_proxy_name.get_slicec('.', 1);

			CRASH_COND(enum_class_name != "Variant"); // Hard-coded...

			generator._log("Declaring global enum '%s' inside struct '%s'\n", enum_proxy_name.utf8().get_data(), enum_class_name.utf8().get_data());

			p_output << "\npublic partial struct " << enum_class_name << "\n" OPEN_BLOCK;
		}

		const String maybe_indent = !enum_in_static_class ? "" : INDENT1;

		if (ienum.is_flags) {
			p_output << "\n"
					 << maybe_indent << "[System.Flags]";
		}

		p_output << "\n"
				 << maybe_indent << "public enum " << enum_proxy_name << " : long"
				 << "\n"
				 << maybe_indent << OPEN_BLOCK;

		for (const ConstantInterface &iconstant : ienum.constants) {
			if (iconstant.const_doc && iconstant.const_doc->description.size()) {
				String xml_summary = generator.bbcode_to_xml(fix_doc_description(iconstant.const_doc->description), nullptr,
						builtin_types, obj_types, enum_types, global_constants, global_enums);
				Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

				if (summary_lines.size()) {
					p_output << maybe_indent << INDENT1 "/// <summary>\n";

					for (int i = 0; i < summary_lines.size(); i++) {
						p_output << maybe_indent << INDENT1 "/// " << summary_lines[i] << "\n";
					}

					p_output << maybe_indent << INDENT1 "/// </summary>\n";
				}
			}

			p_output << maybe_indent << INDENT1
					 << iconstant.proxy_name
					 << " = "
					 << itos(iconstant.value)
					 << ",\n";
		}

		p_output << maybe_indent << CLOSE_BLOCK;

		if (enum_in_static_class) {
			p_output << CLOSE_BLOCK;
		}
	}
}

Error BindingsGenerator::generate_cs_core_project(const String &p_proj_dir) {
	ERR_FAIL_COND_V(!initialized, ERR_UNCONFIGURED);

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	ERR_FAIL_COND_V(da.is_null(), ERR_CANT_CREATE);

	if (!DirAccess::exists(p_proj_dir)) {
		Error err = da->make_dir_recursive(p_proj_dir);
		ERR_FAIL_COND_V_MSG(err != OK, ERR_CANT_CREATE, "Cannot create directory '" + p_proj_dir + "'.");
	}

	da->change_dir(p_proj_dir);
	da->make_dir("Generated");
	da->make_dir("Generated/GodotObjects");

	String base_gen_dir = Path::join(p_proj_dir, "Generated");
	String godot_objects_gen_dir = Path::join(base_gen_dir, "GodotObjects");

	Vector<String> compile_items;

	// Generate source file for global scope constants and enums
	{
		StringBuilder constants_source;
		_generate_global_constants(constants_source);
		String output_file = Path::join(base_gen_dir, BINDINGS_GLOBAL_SCOPE_CLASS "_constants.cs");
		Error save_err = generator._save_file(output_file, constants_source);
		if (save_err != OK) {
			return save_err;
		}

		compile_items.push_back(output_file);
	}

	// Generate source file for array extensions
	{
		StringBuilder extensions_source;
		_generate_array_extensions(extensions_source);
		String output_file = Path::join(base_gen_dir, BINDINGS_GLOBAL_SCOPE_CLASS "_extensions.cs");
		Error save_err = generator._save_file(output_file, extensions_source);
		if (save_err != OK) {
			return save_err;
		}

		compile_items.push_back(output_file);
	}

	for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
		const TypeInterface &itype = E.value;

		if (itype.api_type == ClassDB::API_EDITOR) {
			continue;
		}

		String output_file = Path::join(godot_objects_gen_dir, itype.proxy_name + ".cs");
		Error err = generator._generate_cs_type(itype, output_file, obj_types, global_constants, global_enums,
				builtin_types, enum_types, method_icalls_map);

		if (err == ERR_SKIP) {
			continue;
		}

		if (err != OK) {
			return err;
		}

		compile_items.push_back(output_file);
	}

	// Generate source file for built-in type constructor dictionary.

	{
		StringBuilder cs_built_in_ctors_content;

		cs_built_in_ctors_content.append("namespace " BINDINGS_NAMESPACE ";\n\n");
		cs_built_in_ctors_content.append("using System;\n"
										 "using System.Collections.Generic;\n"
										 "\n");
		cs_built_in_ctors_content.append("internal static class " BINDINGS_CLASS_CONSTRUCTOR "\n{");

		cs_built_in_ctors_content.append(MEMBER_BEGIN "internal static readonly Dictionary<string, Func<IntPtr, GodotObject>> " BINDINGS_CLASS_CONSTRUCTOR_DICTIONARY ";\n");

		cs_built_in_ctors_content.append(MEMBER_BEGIN "public static GodotObject Invoke(string nativeTypeNameStr, IntPtr nativeObjectPtr)\n");
		cs_built_in_ctors_content.append(INDENT1 OPEN_BLOCK);
		cs_built_in_ctors_content.append(INDENT2 "if (!" BINDINGS_CLASS_CONSTRUCTOR_DICTIONARY ".TryGetValue(nativeTypeNameStr, out var constructor))\n");
		cs_built_in_ctors_content.append(INDENT3 "throw new InvalidOperationException(\"Wrapper class not found for type: \" + nativeTypeNameStr);\n");
		cs_built_in_ctors_content.append(INDENT2 "return constructor(nativeObjectPtr);\n");
		cs_built_in_ctors_content.append(INDENT1 CLOSE_BLOCK);

		cs_built_in_ctors_content.append(MEMBER_BEGIN "static " BINDINGS_CLASS_CONSTRUCTOR "()\n");
		cs_built_in_ctors_content.append(INDENT1 OPEN_BLOCK);
		cs_built_in_ctors_content.append(INDENT2 BINDINGS_CLASS_CONSTRUCTOR_DICTIONARY " = new();\n");

		for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
			const TypeInterface &itype = E.value;

			if (itype.api_type != ClassDB::API_CORE || itype.is_singleton_instance) {
				continue;
			}

			if (itype.is_deprecated) {
				cs_built_in_ctors_content.append("#pragma warning disable CS0618\n");
			}

			cs_built_in_ctors_content.append(INDENT2 BINDINGS_CLASS_CONSTRUCTOR_DICTIONARY ".Add(\"");
			cs_built_in_ctors_content.append(itype.name);
			cs_built_in_ctors_content.append("\", " CS_PARAM_INSTANCE " => new ");
			cs_built_in_ctors_content.append(itype.proxy_name);
			if (itype.is_singleton && !itype.is_compat_singleton) {
				cs_built_in_ctors_content.append("Instance");
			}
			cs_built_in_ctors_content.append("(" CS_PARAM_INSTANCE "));\n");

			if (itype.is_deprecated) {
				cs_built_in_ctors_content.append("#pragma warning restore CS0618\n");
			}
		}

		cs_built_in_ctors_content.append(INDENT1 CLOSE_BLOCK);

		cs_built_in_ctors_content.append(CLOSE_BLOCK);

		String constructors_file = Path::join(base_gen_dir, BINDINGS_CLASS_CONSTRUCTOR ".cs");
		Error err = generator._save_file(constructors_file, cs_built_in_ctors_content);

		if (err != OK) {
			return err;
		}

		compile_items.push_back(constructors_file);
	}

	// Generate native calls

	StringBuilder cs_icalls_content;

	cs_icalls_content.append("namespace " BINDINGS_NAMESPACE ";\n\n");
	cs_icalls_content.append("using System;\n"
							 "using System.Diagnostics.CodeAnalysis;\n"
							 "using System.Runtime.InteropServices;\n"
							 "using Godot.NativeInterop;\n"
							 "\n");
	cs_icalls_content.append("[SuppressMessage(\"ReSharper\", \"InconsistentNaming\")]\n");
	cs_icalls_content.append("[SuppressMessage(\"ReSharper\", \"RedundantUnsafeContext\")]\n");
	cs_icalls_content.append("[SuppressMessage(\"ReSharper\", \"RedundantNameQualifier\")]\n");
	cs_icalls_content.append("[System.Runtime.CompilerServices.SkipLocalsInit]\n");
	cs_icalls_content.append("internal static class " BINDINGS_CLASS_NATIVECALLS "\n{");

	cs_icalls_content.append(MEMBER_BEGIN "internal static ulong godot_api_hash = ");
	cs_icalls_content.append(String::num_uint64(ClassDB::get_api_hash(ClassDB::API_CORE)) + ";\n");

	cs_icalls_content.append(MEMBER_BEGIN "private const int VarArgsSpanThreshold = 10;\n");

	for (const InternalCall &icall : method_icalls) {
		if (icall.editor_only) {
			continue;
		}
		Error err = _generate_cs_native_calls(icall, cs_icalls_content);
		if (err != OK) {
			return err;
		}
	}

	cs_icalls_content.append(CLOSE_BLOCK);

	String internal_methods_file = Path::join(base_gen_dir, BINDINGS_CLASS_NATIVECALLS ".cs");

	Error err = generator._save_file(internal_methods_file, cs_icalls_content);
	if (err != OK) {
		return err;
	}

	compile_items.push_back(internal_methods_file);

	// Generate GeneratedIncludes.props

	StringBuilder includes_props_content;
	includes_props_content.append("<Project>\n"
								  "  <ItemGroup>\n");

	for (int i = 0; i < compile_items.size(); i++) {
		String include = Path::relative_to(compile_items[i], p_proj_dir).replace_char('/', '\\');
		includes_props_content.append("    <Compile Include=\"" + include + "\" />\n");
	}

	includes_props_content.append("  </ItemGroup>\n"
								  "</Project>\n");

	String includes_props_file = Path::join(base_gen_dir, "GeneratedIncludes.props");

	err = generator._save_file(includes_props_file, includes_props_content);
	if (err != OK) {
		return err;
	}

	return OK;
}

Error BindingsGenerator::generate_cs_editor_project(const String &p_proj_dir) {
	ERR_FAIL_COND_V(!initialized, ERR_UNCONFIGURED);

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	ERR_FAIL_COND_V(da.is_null(), ERR_CANT_CREATE);

	if (!DirAccess::exists(p_proj_dir)) {
		Error err = da->make_dir_recursive(p_proj_dir);
		ERR_FAIL_COND_V(err != OK, ERR_CANT_CREATE);
	}

	da->change_dir(p_proj_dir);
	da->make_dir("Generated");
	da->make_dir("Generated/GodotObjects");

	String base_gen_dir = Path::join(p_proj_dir, "Generated");
	String godot_objects_gen_dir = Path::join(base_gen_dir, "GodotObjects");

	Vector<String> compile_items;

	for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
		const TypeInterface &itype = E.value;

		if (itype.api_type != ClassDB::API_EDITOR) {
			continue;
		}

		String output_file = Path::join(godot_objects_gen_dir, itype.proxy_name + ".cs");
		Error err = generator._generate_cs_type(itype, output_file, obj_types, global_constants, global_enums,
				builtin_types, enum_types, method_icalls_map);

		if (err == ERR_SKIP) {
			continue;
		}

		if (err != OK) {
			return err;
		}

		compile_items.push_back(output_file);
	}

	// Generate source file for editor type constructor dictionary.

	{
		StringBuilder cs_built_in_ctors_content;

		cs_built_in_ctors_content.append("namespace " BINDINGS_NAMESPACE ";\n\n");
		cs_built_in_ctors_content.append("internal static class " BINDINGS_CLASS_CONSTRUCTOR_EDITOR "\n{");

		cs_built_in_ctors_content.append(MEMBER_BEGIN "private static void AddEditorConstructors()\n");
		cs_built_in_ctors_content.append(INDENT1 OPEN_BLOCK);
		cs_built_in_ctors_content.append(INDENT2 "var builtInMethodConstructors = " BINDINGS_CLASS_CONSTRUCTOR "." BINDINGS_CLASS_CONSTRUCTOR_DICTIONARY ";\n");

		for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
			const TypeInterface &itype = E.value;

			if (itype.api_type != ClassDB::API_EDITOR || itype.is_singleton_instance) {
				continue;
			}

			if (itype.is_deprecated) {
				cs_built_in_ctors_content.append("#pragma warning disable CS0618\n");
			}

			cs_built_in_ctors_content.append(INDENT2 "builtInMethodConstructors.Add(\"");
			cs_built_in_ctors_content.append(itype.name);
			cs_built_in_ctors_content.append("\", " CS_PARAM_INSTANCE " => new ");
			cs_built_in_ctors_content.append(itype.proxy_name);
			if (itype.is_singleton && !itype.is_compat_singleton) {
				cs_built_in_ctors_content.append("Instance");
			}
			cs_built_in_ctors_content.append("(" CS_PARAM_INSTANCE "));\n");

			if (itype.is_deprecated) {
				cs_built_in_ctors_content.append("#pragma warning restore CS0618\n");
			}
		}

		cs_built_in_ctors_content.append(INDENT1 CLOSE_BLOCK);

		cs_built_in_ctors_content.append(CLOSE_BLOCK);

		String constructors_file = Path::join(base_gen_dir, BINDINGS_CLASS_CONSTRUCTOR_EDITOR ".cs");
		Error err = generator._save_file(constructors_file, cs_built_in_ctors_content);

		if (err != OK) {
			return err;
		}

		compile_items.push_back(constructors_file);
	}

	// Generate native calls

	StringBuilder cs_icalls_content;

	cs_icalls_content.append("namespace " BINDINGS_NAMESPACE ";\n\n");
	cs_icalls_content.append("using System;\n"
							 "using System.Diagnostics.CodeAnalysis;\n"
							 "using System.Runtime.InteropServices;\n"
							 "using Godot.NativeInterop;\n"
							 "\n");
	cs_icalls_content.append("[SuppressMessage(\"ReSharper\", \"InconsistentNaming\")]\n");
	cs_icalls_content.append("[SuppressMessage(\"ReSharper\", \"RedundantUnsafeContext\")]\n");
	cs_icalls_content.append("[SuppressMessage(\"ReSharper\", \"RedundantNameQualifier\")]\n");
	cs_icalls_content.append("[System.Runtime.CompilerServices.SkipLocalsInit]\n");
	cs_icalls_content.append("internal static class " BINDINGS_CLASS_NATIVECALLS_EDITOR "\n" OPEN_BLOCK);

	cs_icalls_content.append(INDENT1 "internal static ulong godot_api_hash = ");
	cs_icalls_content.append(String::num_uint64(ClassDB::get_api_hash(ClassDB::API_EDITOR)) + ";\n");

	cs_icalls_content.append(MEMBER_BEGIN "private const int VarArgsSpanThreshold = 10;\n");

	cs_icalls_content.append("\n");

	for (const InternalCall &icall : method_icalls) {
		if (!icall.editor_only) {
			continue;
		}
		Error err = _generate_cs_native_calls(icall, cs_icalls_content);
		if (err != OK) {
			return err;
		}
	}

	cs_icalls_content.append(CLOSE_BLOCK);

	String internal_methods_file = Path::join(base_gen_dir, BINDINGS_CLASS_NATIVECALLS_EDITOR ".cs");

	Error err = generator._save_file(internal_methods_file, cs_icalls_content);
	if (err != OK) {
		return err;
	}

	compile_items.push_back(internal_methods_file);

	// Generate GeneratedIncludes.props

	StringBuilder includes_props_content;
	includes_props_content.append("<Project>\n"
								  "  <ItemGroup>\n");

	for (int i = 0; i < compile_items.size(); i++) {
		String include = Path::relative_to(compile_items[i], p_proj_dir).replace_char('/', '\\');
		includes_props_content.append("    <Compile Include=\"" + include + "\" />\n");
	}

	includes_props_content.append("  </ItemGroup>\n"
								  "</Project>\n");

	String includes_props_file = Path::join(base_gen_dir, "GeneratedIncludes.props");

	err = generator._save_file(includes_props_file, includes_props_content);
	if (err != OK) {
		return err;
	}

	return OK;
}

Error BindingsGenerator::generate_cs_api(const String &p_output_dir) {
	ERR_FAIL_COND_V(!initialized, ERR_UNCONFIGURED);

	String output_dir = Path::abspath(Path::realpath(p_output_dir));

	Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	ERR_FAIL_COND_V(da.is_null(), ERR_CANT_CREATE);

	if (!DirAccess::exists(output_dir)) {
		Error err = da->make_dir_recursive(output_dir);
		ERR_FAIL_COND_V(err != OK, ERR_CANT_CREATE);
	}

	Error proj_err;

	// Generate GodotSharp source files

	String core_proj_dir = output_dir.path_join(CORE_API_ASSEMBLY_NAME);

	proj_err = generate_cs_core_project(core_proj_dir);
	if (proj_err != OK) {
		ERR_PRINT("Generation of the Core API C# project failed.");
		return proj_err;
	}

	// Generate GodotSharpEditor source files

	String editor_proj_dir = output_dir.path_join(EDITOR_API_ASSEMBLY_NAME);

	proj_err = generate_cs_editor_project(editor_proj_dir);
	if (proj_err != OK) {
		ERR_PRINT("Generation of the Editor API C# project failed.");
		return proj_err;
	}

	generator._log("The Godot API sources were successfully generated\n");

	return OK;
}

Error BindingsGenerator::_generate_cs_property(const TypeInterface &p_itype, const PropertyInterface &p_iprop, StringBuilder &p_output) {
	const MethodInterface *setter = p_itype.find_method_by_name(p_iprop.setter);

	// Search it in base types too
	const TypeInterface *current_type = &p_itype;
	while (!setter && current_type->base_name != StringName()) {
		HashMap<StringName, TypeInterface>::Iterator base_match = obj_types.find(current_type->base_name);
		ERR_FAIL_COND_V_MSG(!base_match, ERR_BUG, "Type not found '" + current_type->base_name + "'. Inherited by '" + current_type->name + "'.");
		current_type = &base_match->value;
		setter = current_type->find_method_by_name(p_iprop.setter);
	}

	const MethodInterface *getter = p_itype.find_method_by_name(p_iprop.getter);

	// Search it in base types too
	current_type = &p_itype;
	while (!getter && current_type->base_name != StringName()) {
		HashMap<StringName, TypeInterface>::Iterator base_match = obj_types.find(current_type->base_name);
		ERR_FAIL_COND_V_MSG(!base_match, ERR_BUG, "Type not found '" + current_type->base_name + "'. Inherited by '" + current_type->name + "'.");
		current_type = &base_match->value;
		getter = current_type->find_method_by_name(p_iprop.getter);
	}

	ERR_FAIL_COND_V(!setter && !getter, ERR_BUG);

	if (setter) {
		int setter_argc = p_iprop.index != -1 ? 2 : 1;
		ERR_FAIL_COND_V(setter->arguments.size() != setter_argc, ERR_BUG);
	}

	if (getter) {
		int getter_argc = p_iprop.index != -1 ? 1 : 0;
		ERR_FAIL_COND_V(getter->arguments.size() != getter_argc, ERR_BUG);
	}

	if (getter && setter) {
		const ArgumentInterface &setter_first_arg = setter->arguments.back()->get();
		if (getter->return_type.cname != setter_first_arg.type.cname) {
			ERR_FAIL_V_MSG(ERR_BUG,
					"Return type from getter doesn't match first argument of setter for property: '" +
							p_itype.name + "." + String(p_iprop.cname) + "'.");
		}
	}

	const TypeReference &proptype_name = getter ? getter->return_type : setter->arguments.back()->get().type;

	const TypeInterface *prop_itype = generator._get_type_or_singleton_or_null(proptype_name,
			builtin_types, obj_types, enum_types);
	ERR_FAIL_NULL_V_MSG(prop_itype, ERR_BUG, "Property type '" + proptype_name.cname + "' was not found.");

	ERR_FAIL_COND_V_MSG(prop_itype->is_singleton, ERR_BUG,
			"Property type is a singleton: '" + p_itype.name + "." + String(p_iprop.cname) + "'.");

	if (p_itype.api_type == ClassDB::API_CORE) {
		ERR_FAIL_COND_V_MSG(prop_itype->api_type == ClassDB::API_EDITOR, ERR_BUG,
				"Property '" + p_itype.name + "." + String(p_iprop.cname) + "' has type '" + prop_itype->name +
						"' from the editor API. Core API cannot have dependencies on the editor API.");
	}

	if (p_iprop.prop_doc && p_iprop.prop_doc->description.size()) {
		String xml_summary = generator.bbcode_to_xml(fix_doc_description(p_iprop.prop_doc->description), &p_itype,
				builtin_types, obj_types, enum_types, global_constants, global_enums);
		Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

		if (summary_lines.size()) {
			p_output.append(MEMBER_BEGIN "/// <summary>\n");

			for (int i = 0; i < summary_lines.size(); i++) {
				p_output.append(INDENT1 "/// ");
				p_output.append(summary_lines[i]);
				p_output.append("\n");
			}

			p_output.append(INDENT1 "/// </summary>");
		}
	}

	if (p_iprop.is_deprecated) {
		p_output.append(MEMBER_BEGIN "[Obsolete(\"");
		p_output.append(generator.bbcode_to_text(p_iprop.deprecation_message, &p_itype, global_constants,
				global_enums, builtin_types, obj_types, enum_types));
		p_output.append("\")]");
	}

	if (p_iprop.is_hidden) {
		p_output.append(MEMBER_BEGIN "[EditorBrowsable(EditorBrowsableState.Never)]");
		// Deprecated PROPERTY_USAGE_INTERNAL properties appear as hidden to C# and may call deprecated getter/setter functions.
		p_output.append("\n#pragma warning disable CS0618 // Type or member is obsolete.");
	}

	p_output.append(MEMBER_BEGIN "public ");

	if (prop_allowed_inherited_member_hiding.has(p_itype.proxy_name + "." + p_iprop.proxy_name)) {
		p_output.append("new ");
	}

	if (p_itype.is_singleton) {
		p_output.append("static ");
	}

	String prop_cs_type = prop_itype->cs_type +
			generator._get_generic_type_parameters(*prop_itype, proptype_name.generic_type_parameters,
					builtin_types, obj_types, enum_types);

	p_output.append(prop_cs_type);
	p_output.append(" ");
	p_output.append(p_iprop.proxy_name);
	p_output.append("\n" OPEN_BLOCK_L1);

	if (getter) {
		p_output.append(INDENT2 "get\n" OPEN_BLOCK_L2 INDENT3);

		p_output.append("return ");
		p_output.append(getter->proxy_name + "(");
		if (p_iprop.index != -1) {
			const ArgumentInterface &idx_arg = getter->arguments.front()->get();
			if (idx_arg.type.cname != generator.name_cache.type_int) {
				// Assume the index parameter is an enum
				const TypeInterface *idx_arg_type = generator._get_type_or_null(idx_arg.type, builtin_types,
						obj_types, enum_types);
				CRASH_COND(idx_arg_type == nullptr);
				p_output.append("(" + idx_arg_type->proxy_name + ")(" + itos(p_iprop.index) + ")");
			} else {
				p_output.append(itos(p_iprop.index));
			}
		}
		p_output.append(");\n" CLOSE_BLOCK_L2);
	}

	if (setter) {
		p_output.append(INDENT2 "set\n" OPEN_BLOCK_L2 INDENT3);

		p_output.append(setter->proxy_name + "(");
		if (p_iprop.index != -1) {
			const ArgumentInterface &idx_arg = setter->arguments.front()->get();
			if (idx_arg.type.cname != generator.name_cache.type_int) {
				// Assume the index parameter is an enum
				const TypeInterface *idx_arg_type = generator._get_type_or_null(idx_arg.type,
						builtin_types, obj_types, enum_types);
				CRASH_COND(idx_arg_type == nullptr);
				p_output.append("(" + idx_arg_type->proxy_name + ")(" + itos(p_iprop.index) + "), ");
			} else {
				p_output.append(itos(p_iprop.index) + ", ");
			}
		}
		p_output.append("value);\n" CLOSE_BLOCK_L2);
	}

	p_output.append(CLOSE_BLOCK_L1);

	if (p_iprop.is_hidden) {
		p_output.append("#pragma warning restore CS0618 // Type or member is obsolete.\n");
	}

	return OK;
}

Error BindingsGenerator::_generate_cs_method(const TypeInterface &p_itype, const MethodInterface &p_imethod, int &p_method_bind_count, StringBuilder &p_output, bool p_use_span) {
	const TypeInterface *return_type = generator._get_type_or_singleton_or_null(p_imethod.return_type,
			builtin_types, obj_types, enum_types);
	ERR_FAIL_NULL_V_MSG(return_type, ERR_BUG, "Return type '" + p_imethod.return_type.cname + "' was not found.");

	ERR_FAIL_COND_V_MSG(return_type->is_singleton, ERR_BUG,
			"Method return type is a singleton: '" + p_itype.name + "." + p_imethod.name + "'.");

	if (p_itype.api_type == ClassDB::API_CORE) {
		ERR_FAIL_COND_V_MSG(return_type->api_type == ClassDB::API_EDITOR, ERR_BUG,
				"Method '" + p_itype.name + "." + p_imethod.name + "' has return type '" + return_type->name +
						"' from the editor API. Core API cannot have dependencies on the editor API.");
	}

	if (p_imethod.is_virtual && p_use_span) {
		return OK;
	}

	bool has_span_argument = false;

	if (p_use_span) {
		if (p_imethod.is_vararg) {
			has_span_argument = true;
		} else {
			for (const ArgumentInterface &iarg : p_imethod.arguments) {
				const TypeInterface *arg_type = generator._get_type_or_singleton_or_null(iarg.type,
						builtin_types, obj_types, enum_types);
				ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

				if (arg_type->is_span_compatible) {
					has_span_argument = true;
					break;
				}
			}
		}

		if (has_span_argument) {
			// Span overloads use the same method bind as the array overloads.
			// Since both overloads are generated one after the other, we can decrease the count here
			// to ensure the span overload uses the same method bind.
			p_method_bind_count--;
		}
	}

	String method_bind_field = CS_STATIC_FIELD_METHOD_BIND_PREFIX + itos(p_method_bind_count);

	String arguments_sig;
	StringBuilder cs_in_statements;
	bool cs_in_expr_is_unsafe = false;

	String icall_params = method_bind_field;

	if (!p_imethod.is_static) {
		String self_reference = "this";
		if (p_itype.is_singleton) {
			self_reference = CS_PROPERTY_SINGLETON;
		}

		if (p_itype.cs_in.size()) {
			cs_in_statements << sformat(p_itype.cs_in, p_itype.c_type, self_reference,
					String(), String(), String(), INDENT2);
		}

		icall_params += ", " + sformat(p_itype.cs_in_expr, self_reference);
	}

	StringBuilder default_args_doc;

	// Retrieve information from the arguments
	const ArgumentInterface &first = p_imethod.arguments.front()->get();
	for (const ArgumentInterface &iarg : p_imethod.arguments) {
		const TypeInterface *arg_type = generator._get_type_or_singleton_or_null(iarg.type, builtin_types,
				obj_types, enum_types);
		ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

		ERR_FAIL_COND_V_MSG(arg_type->is_singleton, ERR_BUG,
				"Argument type is a singleton: '" + iarg.name + "' of method '" + p_itype.name + "." + p_imethod.name + "'.");

		if (p_itype.api_type == ClassDB::API_CORE) {
			ERR_FAIL_COND_V_MSG(arg_type->api_type == ClassDB::API_EDITOR, ERR_BUG,
					"Argument '" + iarg.name + "' of method '" + p_itype.name + "." + p_imethod.name + "' has type '" +
							arg_type->name + "' from the editor API. Core API cannot have dependencies on the editor API.");
		}

		if (iarg.default_argument.size()) {
			CRASH_COND_MSG(!generator._arg_default_value_is_assignable_to_type(iarg.def_param_value, *arg_type),
					"Invalid default value for parameter '" + iarg.name + "' of method '" + p_itype.name + "." + p_imethod.name + "'.");
		}

		String arg_cs_type = arg_type->cs_type +
				generator._get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
						builtin_types, obj_types, enum_types);

		bool use_span_for_arg = p_use_span && arg_type->is_span_compatible;

		// Add the current arguments to the signature
		// If the argument has a default value which is not a constant, we will make it Nullable
		{
			if (&iarg != &first) {
				arguments_sig += ", ";
			}

			if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
				arguments_sig += "Nullable<";
			}

			if (use_span_for_arg) {
				arguments_sig += arg_type->c_type_in;
			} else {
				arguments_sig += arg_cs_type;
			}

			if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
				arguments_sig += "> ";
			} else {
				arguments_sig += " ";
			}

			arguments_sig += iarg.name;

			if (!p_use_span && !p_imethod.is_compat && iarg.default_argument.size()) {
				if (iarg.def_param_mode != ArgumentInterface::CONSTANT) {
					arguments_sig += " = null";
				} else {
					arguments_sig += " = " + sformat(iarg.default_argument, arg_type->cs_type);
				}
			}
		}

		icall_params += ", ";

		if (iarg.default_argument.size() && iarg.def_param_mode != ArgumentInterface::CONSTANT && !use_span_for_arg) {
			// The default value of an argument must be constant. Otherwise we make it Nullable and do the following:
			// Type arg_in = arg.HasValue ? arg.Value : <non-const default value>;
			String arg_or_defval_local = iarg.name;
			arg_or_defval_local += "OrDefVal";

			cs_in_statements << INDENT2 << arg_cs_type << " " << arg_or_defval_local << " = " << iarg.name;

			if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
				cs_in_statements << ".HasValue ? ";
			} else {
				cs_in_statements << " != null ? ";
			}

			cs_in_statements << iarg.name;

			if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
				cs_in_statements << ".Value : ";
			} else {
				cs_in_statements << " : ";
			}

			String cs_type = arg_cs_type;
			if (cs_type.ends_with("[]")) {
				cs_type = cs_type.substr(0, cs_type.length() - 2);
			}

			String def_arg = sformat(iarg.default_argument, cs_type);

			cs_in_statements << def_arg << ";\n";

			if (arg_type->cs_in.size()) {
				cs_in_statements << sformat(arg_type->cs_in, arg_type->c_type, arg_or_defval_local,
						String(), String(), String(), INDENT2);
			}

			if (arg_type->cs_in_expr.is_empty()) {
				icall_params += arg_or_defval_local;
			} else {
				icall_params += sformat(arg_type->cs_in_expr, arg_or_defval_local, arg_type->c_type);
			}

			// Apparently the name attribute must not include the @
			String param_tag_name = iarg.name.begins_with("@") ? iarg.name.substr(1) : iarg.name;
			// Escape < and > in the attribute default value
			String param_def_arg = def_arg.replacen("<", "&lt;").replacen(">", "&gt;");

			default_args_doc.append(MEMBER_BEGIN "/// <param name=\"" + param_tag_name + "\">If the parameter is null, then the default value is <c>" + param_def_arg + "</c>.</param>");
		} else {
			if (arg_type->cs_in.size()) {
				cs_in_statements << sformat(arg_type->cs_in, arg_type->c_type, iarg.name,
						String(), String(), String(), INDENT2);
			}

			icall_params += arg_type->cs_in_expr.is_empty() ? iarg.name : sformat(arg_type->cs_in_expr, iarg.name, arg_type->c_type);
		}

		cs_in_expr_is_unsafe |= arg_type->cs_in_expr_is_unsafe;
	}

	if (p_use_span && !has_span_argument) {
		return OK;
	}

	// Collect caller name for MethodBind
	if (p_imethod.is_vararg) {
		icall_params += ", (godot_string_name)MethodName." + p_imethod.proxy_name + ".NativeValue";
	}

	// Generate method
	{
		if (!p_imethod.is_virtual && !p_imethod.requires_object_call && !p_use_span) {
			p_output << MEMBER_BEGIN "[DebuggerBrowsable(DebuggerBrowsableState.Never)]\n"
					 << INDENT1 "private static readonly IntPtr " << method_bind_field << " = ";

			if (p_itype.is_singleton) {
				// Singletons are static classes. They don't derive GodotObject,
				// so we need to specify the type to call the static method.
				p_output << "GodotObject.";
			}

			p_output << ICALL_CLASSDB_GET_METHOD_WITH_COMPATIBILITY "(" BINDINGS_NATIVE_NAME_FIELD ", MethodName."
					 << p_imethod.proxy_name << ", " << itos(p_imethod.hash) << "ul"
					 << ");\n";
		}

		if (p_imethod.method_doc && p_imethod.method_doc->description.size()) {
			String xml_summary = generator.bbcode_to_xml(fix_doc_description(p_imethod.method_doc->description),
					&p_itype, builtin_types, obj_types, enum_types, global_constants, global_enums);
			Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

			if (summary_lines.size()) {
				p_output.append(MEMBER_BEGIN "/// <summary>\n");

				for (int i = 0; i < summary_lines.size(); i++) {
					p_output.append(INDENT1 "/// ");
					p_output.append(summary_lines[i]);
					p_output.append("\n");
				}

				p_output.append(INDENT1 "/// </summary>");
			}
		}

		if (default_args_doc.get_string_length()) {
			p_output.append(default_args_doc.as_string());
		}

		if (p_imethod.is_deprecated) {
			p_output.append(MEMBER_BEGIN "[Obsolete(\"");
			p_output.append(generator.bbcode_to_text(p_imethod.deprecation_message, &p_itype, global_constants,
					global_enums, builtin_types, obj_types, enum_types));
			p_output.append("\")]");
		}

		if (p_imethod.is_hidden) {
			p_output.append(MEMBER_BEGIN "[EditorBrowsable(EditorBrowsableState.Never)]");
		}

		p_output.append(MEMBER_BEGIN);
		p_output.append(p_imethod.is_internal ? "internal " : "public ");

		if (prop_allowed_inherited_member_hiding.has(p_itype.proxy_name + "." + p_imethod.proxy_name)) {
			p_output.append("new ");
		}

		if (p_itype.is_singleton || p_imethod.is_static) {
			p_output.append("static ");
		} else if (p_imethod.is_virtual) {
			p_output.append("virtual ");
		}

		if (cs_in_expr_is_unsafe) {
			p_output.append("unsafe ");
		}

		String return_cs_type = return_type->cs_type +
				generator._get_generic_type_parameters(*return_type, p_imethod.return_type.generic_type_parameters,
						builtin_types, obj_types, enum_types);

		p_output.append(return_cs_type + " ");
		p_output.append(p_imethod.proxy_name + "(");
		p_output.append(arguments_sig + ")\n" OPEN_BLOCK_L1);

		if (p_imethod.is_virtual) {
			// Godot virtual method must be overridden, therefore we return a default value by default.

			if (return_type->cname == generator.name_cache.type_void) {
				p_output.append(CLOSE_BLOCK_L1);
			} else {
				p_output.append(INDENT2 "return default;\n" CLOSE_BLOCK_L1);
			}

			return OK; // Won't increment method bind count
		}

		if (p_imethod.requires_object_call) {
			// Fallback to Godot's object.Call(string, params)

			p_output.append(INDENT2 CS_METHOD_CALL "(");
			p_output.append("MethodName." + p_imethod.proxy_name);

			for (const ArgumentInterface &iarg : p_imethod.arguments) {
				p_output.append(", ");
				p_output.append(iarg.name);
			}

			p_output.append(");\n" CLOSE_BLOCK_L1);

			return OK; // Won't increment method bind count
		}

		HashMap<const MethodInterface *, const InternalCall *>::ConstIterator match = method_icalls_map.find(&p_imethod);
		ERR_FAIL_NULL_V(match, ERR_BUG);

		const InternalCall *im_icall = match->value;

		String im_call = im_icall->editor_only ? BINDINGS_CLASS_NATIVECALLS_EDITOR : BINDINGS_CLASS_NATIVECALLS;
		im_call += ".";
		im_call += im_icall->name;

		if (p_imethod.arguments.size() && cs_in_statements.get_string_length() > 0) {
			p_output.append(cs_in_statements.as_string());
		}

		if (return_type->cname == generator.name_cache.type_void) {
			p_output << INDENT2 << im_call << "(" << icall_params << ");\n";
		} else if (return_type->cs_out.is_empty()) {
			p_output << INDENT2 "return " << im_call << "(" << icall_params << ");\n";
		} else {
			p_output.append(sformat(return_type->cs_out, im_call, icall_params,
					return_cs_type, return_type->c_type_out, String(), INDENT2));
			p_output.append("\n");
		}

		p_output.append(CLOSE_BLOCK_L1);
	}

	p_method_bind_count++;

	return OK;
}

Error BindingsGenerator::_generate_cs_signal(const TypeInterface &p_itype, const SignalInterface &p_isignal, StringBuilder &p_output) {
	String arguments_sig;

	// Retrieve information from the arguments
	const ArgumentInterface &first = p_isignal.arguments.front()->get();
	for (const ArgumentInterface &iarg : p_isignal.arguments) {
		const TypeInterface *arg_type = generator._get_type_or_singleton_or_null(iarg.type,
				builtin_types, obj_types, enum_types);
		ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

		ERR_FAIL_COND_V_MSG(arg_type->is_singleton, ERR_BUG,
				"Argument type is a singleton: '" + iarg.name + "' of signal '" + p_itype.name + "." + p_isignal.name + "'.");

		if (p_itype.api_type == ClassDB::API_CORE) {
			ERR_FAIL_COND_V_MSG(arg_type->api_type == ClassDB::API_EDITOR, ERR_BUG,
					"Argument '" + iarg.name + "' of signal '" + p_itype.name + "." + p_isignal.name + "' has type '" +
							arg_type->name + "' from the editor API. Core API cannot have dependencies on the editor API.");
		}

		// Add the current arguments to the signature

		if (&iarg != &first) {
			arguments_sig += ", ";
		}

		String arg_cs_type = arg_type->cs_type +
				generator._get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
						builtin_types, obj_types, enum_types);

		arguments_sig += arg_cs_type;
		arguments_sig += " ";
		arguments_sig += iarg.name;
	}

	// Generate signal
	{
		bool is_parameterless = p_isignal.arguments.is_empty();

		// Delegate name is [SignalName]EventHandler
		String delegate_name = is_parameterless ? "Action" : p_isignal.proxy_name + "EventHandler";

		if (!is_parameterless) {
			p_output.append(MEMBER_BEGIN "/// <summary>\n");
			p_output.append(INDENT1 "/// ");
			p_output.append("Represents the method that handles the ");
			p_output.append("<see cref=\"" BINDINGS_NAMESPACE "." + p_itype.proxy_name + "." + p_isignal.proxy_name + "\"/>");
			p_output.append(" event of a ");
			p_output.append("<see cref=\"" BINDINGS_NAMESPACE "." + p_itype.proxy_name + "\"/>");
			p_output.append(" class.\n");
			p_output.append(INDENT1 "/// </summary>");

			// Generate delegate
			if (p_isignal.is_deprecated) {
				p_output.append(MEMBER_BEGIN "[Obsolete(\"");
				p_output.append(generator.bbcode_to_text(p_isignal.deprecation_message, &p_itype, global_constants,
						global_enums, builtin_types, obj_types, enum_types));
				p_output.append("\")]");
			}
			p_output.append(MEMBER_BEGIN "public delegate void ");
			p_output.append(delegate_name);
			p_output.append("(");
			p_output.append(arguments_sig);
			p_output.append(");\n");

			// Generate Callable trampoline for the delegate
			if (p_isignal.is_deprecated) {
				p_output.append(MEMBER_BEGIN "[Obsolete(\"");
				p_output.append(generator.bbcode_to_text(p_isignal.deprecation_message, &p_itype, global_constants,
						global_enums, builtin_types, obj_types, enum_types));
				p_output.append("\")]");
			}
			p_output << MEMBER_BEGIN "private static void " << p_isignal.proxy_name << "Trampoline"
					 << "(object delegateObj, NativeVariantPtrArgs args, out godot_variant ret)\n"
					 << INDENT1 "{\n"
					 << INDENT2 "Callable.ThrowIfArgCountMismatch(args, " << itos(p_isignal.arguments.size()) << ");\n"
					 << INDENT2 "((" << delegate_name << ")delegateObj)(";

			int idx = 0;
			for (const ArgumentInterface &iarg : p_isignal.arguments) {
				const TypeInterface *arg_type = generator._get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);
				ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

				if (idx != 0) {
					p_output << ", ";
				}

				if (arg_type->cname == generator.name_cache.type_Array_generic || arg_type->cname == generator.name_cache.type_Dictionary_generic) {
					String arg_cs_type = arg_type->cs_type +
							generator._get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
									builtin_types, obj_types, enum_types);

					p_output << "new " << arg_cs_type << "(" << sformat(arg_type->cs_variant_to_managed, "args[" + itos(idx) + "]", arg_type->cs_type, arg_type->name) << ")";
				} else {
					p_output << sformat(arg_type->cs_variant_to_managed,
							"args[" + itos(idx) + "]", arg_type->cs_type, arg_type->name);
				}

				idx++;
			}

			p_output << ");\n"
					 << INDENT2 "ret = default;\n"
					 << INDENT1 "}\n";
		}

		if (p_isignal.method_doc && p_isignal.method_doc->description.size()) {
			String xml_summary = generator.bbcode_to_xml(fix_doc_description(p_isignal.method_doc->description), &p_itype,
					builtin_types, obj_types, enum_types, global_constants, global_enums, true);
			Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

			if (summary_lines.size()) {
				p_output.append(MEMBER_BEGIN "/// <summary>\n");

				for (int i = 0; i < summary_lines.size(); i++) {
					p_output.append(INDENT1 "/// ");
					p_output.append(summary_lines[i]);
					p_output.append("\n");
				}

				p_output.append(INDENT1 "/// </summary>");
			}
		}

		// TODO:
		// Could we assume the StringName instance of signal name will never be freed (it's stored in ClassDB) before the managed world is unloaded?
		// If so, we could store the pointer we get from `data_unique_pointer()` instead of allocating StringName here.

		// Generate event
		if (p_isignal.is_deprecated) {
			p_output.append(MEMBER_BEGIN "[Obsolete(\"");
			p_output.append(generator.bbcode_to_text(p_isignal.deprecation_message, &p_itype, global_constants,
					global_enums, builtin_types, obj_types, enum_types));
			p_output.append("\")]");
		}
		p_output.append(MEMBER_BEGIN "public ");

		if (p_itype.is_singleton) {
			p_output.append("static ");
		}

		if (!is_parameterless) {
			// `unsafe` is needed for taking the trampoline's function pointer
			p_output << "unsafe ";
		}

		p_output.append("event ");
		p_output.append(delegate_name);
		p_output.append(" ");
		p_output.append(p_isignal.proxy_name);
		p_output.append("\n" OPEN_BLOCK_L1 INDENT2);

		if (p_itype.is_singleton) {
			p_output.append("add => " CS_PROPERTY_SINGLETON ".Connect(SignalName.");
		} else {
			p_output.append("add => Connect(SignalName.");
		}

		if (is_parameterless) {
			// Delegate type is Action. No need for custom trampoline.
			p_output << p_isignal.proxy_name << ", Callable.From(value));\n";
		} else {
			p_output << p_isignal.proxy_name
					 << ", Callable.CreateWithUnsafeTrampoline(value, &" << p_isignal.proxy_name << "Trampoline));\n";
		}

		if (p_itype.is_singleton) {
			p_output.append(INDENT2 "remove => " CS_PROPERTY_SINGLETON ".Disconnect(SignalName.");
		} else {
			p_output.append(INDENT2 "remove => Disconnect(SignalName.");
		}

		if (is_parameterless) {
			// Delegate type is Action. No need for custom trampoline.
			p_output << p_isignal.proxy_name << ", Callable.From(value));\n";
		} else {
			p_output << p_isignal.proxy_name
					 << ", Callable.CreateWithUnsafeTrampoline(value, &" << p_isignal.proxy_name << "Trampoline));\n";
		}

		p_output.append(CLOSE_BLOCK_L1);

		// Generate EmitSignal{EventName} method to raise the event.
		if (!p_itype.is_singleton) {
			if (p_isignal.is_deprecated) {
				p_output.append(MEMBER_BEGIN "[Obsolete(\"");
				p_output.append(generator.bbcode_to_text(p_isignal.deprecation_message, &p_itype,
						global_constants, global_enums, builtin_types, obj_types, enum_types));
				p_output.append("\")]");
			}
			p_output.append(MEMBER_BEGIN "protected void ");
			p_output << "EmitSignal" << p_isignal.proxy_name;
			if (is_parameterless) {
				p_output.append("()\n" OPEN_BLOCK_L1 INDENT2);
				p_output << "EmitSignal(SignalName." << p_isignal.proxy_name << ");\n";
				p_output.append(CLOSE_BLOCK_L1);
			} else {
				p_output.append("(");

				StringBuilder cs_emitsignal_params;

				int idx = 0;
				for (const ArgumentInterface &iarg : p_isignal.arguments) {
					const TypeInterface *arg_type = generator._get_type_or_null(iarg.type,
							builtin_types, obj_types, enum_types);
					ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

					if (idx != 0) {
						p_output << ", ";
						cs_emitsignal_params << ", ";
					}

					String arg_cs_type = arg_type->cs_type +
							generator._get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
									builtin_types, obj_types, enum_types);

					p_output << arg_cs_type << " " << iarg.name;

					if (arg_type->is_enum) {
						cs_emitsignal_params << "(long)";
					}

					cs_emitsignal_params << iarg.name;

					idx++;
				}

				p_output.append(")\n" OPEN_BLOCK_L1 INDENT2);
				p_output << "EmitSignal(SignalName." << p_isignal.proxy_name << ", " << cs_emitsignal_params << ");\n";
				p_output.append(CLOSE_BLOCK_L1);
			}
		}
	}

	return OK;
}

Error BindingsGenerator::_generate_cs_native_calls(const InternalCall &p_icall, StringBuilder &r_output) {
	bool ret_void = p_icall.return_type.cname == generator.name_cache.type_void;

	const TypeInterface *return_type = generator._get_type_or_null(p_icall.return_type,
			builtin_types, obj_types, enum_types);
	ERR_FAIL_NULL_V_MSG(return_type, ERR_BUG, "Return type '" + p_icall.return_type.cname + "' was not found.");

	StringBuilder c_func_sig;
	StringBuilder c_in_statements;
	StringBuilder c_args_var_content;

	c_func_sig << "IntPtr " CS_PARAM_METHODBIND;

	if (!p_icall.is_static) {
		c_func_sig += ", IntPtr " CS_PARAM_INSTANCE;
	}

	// Get arguments information
	int i = 0;
	for (const TypeReference &arg_type_ref : p_icall.argument_types) {
		const TypeInterface *arg_type = generator._get_type_or_null(arg_type_ref,
				builtin_types, obj_types, enum_types);
		ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + arg_type_ref.cname + "' was not found.");

		String c_param_name = "arg" + itos(i + 1);

		if (p_icall.is_vararg) {
			if (i < p_icall.get_arguments_count() - 1) {
				String c_in_vararg = arg_type->c_in_vararg;

				if (arg_type->is_object_type) {
					c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromGodotObjectPtr(%1);\n";
				}

				ERR_FAIL_COND_V_MSG(c_in_vararg.is_empty(), ERR_BUG,
						"VarArg support not implemented for parameter type: " + arg_type->name);

				c_in_statements
						<< sformat(c_in_vararg, return_type->c_type, c_param_name,
								   String(), String(), String(), INDENT3)
						<< INDENT3 C_LOCAL_PTRCALL_ARGS "[" << itos(i)
						<< "] = new IntPtr(&" << c_param_name << "_in);\n";
			}
		} else {
			if (i > 0) {
				c_args_var_content << ", ";
			}
			if (arg_type->c_in.size()) {
				c_in_statements << sformat(arg_type->c_in, arg_type->c_type, c_param_name,
						String(), String(), String(), INDENT2);
			}
			c_args_var_content << sformat(arg_type->c_arg_in, c_param_name);
		}

		c_func_sig << ", " << arg_type->c_type_in << " " << c_param_name;

		i++;
	}

	// Collect caller name for MethodBind
	if (p_icall.is_vararg) {
		c_func_sig << ", godot_string_name caller";
	}

	String icall_method = p_icall.name;

	// Generate icall function

	r_output << MEMBER_BEGIN "internal static unsafe " << (ret_void ? "void" : return_type->c_type_out) << " "
			 << icall_method << "(" << c_func_sig.as_string() << ")\n" OPEN_BLOCK_L1;

	if (!p_icall.is_static) {
		r_output << INDENT2 "ExceptionUtils.ThrowIfNullPtr(" CS_PARAM_INSTANCE ");\n";
	}

	if (!ret_void && (!p_icall.is_vararg || return_type->cname != generator.name_cache.type_Variant)) {
		String ptrcall_return_type;
		String initialization;

		if (return_type->is_object_type) {
			ptrcall_return_type = return_type->is_ref_counted ? "godot_ref" : return_type->c_type;
			initialization = " = default";
		} else {
			ptrcall_return_type = return_type->c_type;
		}

		r_output << INDENT2;

		if (return_type->is_ref_counted || return_type->c_type_is_disposable_struct) {
			r_output << "using ";

			if (initialization.is_empty()) {
				initialization = " = default";
			}
		} else if (return_type->c_ret_needs_default_initialization) {
			initialization = " = default";
		}

		r_output << ptrcall_return_type << " " C_LOCAL_RET << initialization << ";\n";
	}

	String argc_str = itos(p_icall.get_arguments_count());

	auto generate_call_and_return_stmts = [&](const char *base_indent) {
		if (p_icall.is_vararg) {
			// MethodBind Call
			r_output << base_indent;

			// VarArg methods always return Variant, but there are some cases in which MethodInfo provides
			// a specific return type. We trust this information is valid. We need a temporary local to keep
			// the Variant alive until the method returns. Otherwise, if the returned Variant holds a RefPtr,
			// it could be deleted too early. This is the case with GDScript.new() which returns OBJECT.
			// Alternatively, we could just return Variant, but that would result in a worse API.

			if (!ret_void) {
				if (return_type->cname != generator.name_cache.type_Variant) {
					// Usually the return value takes ownership, but in this case the variant is only used
					// for conversion to another return type. As such, the local variable takes ownership.
					r_output << "using godot_variant " << C_LOCAL_VARARG_RET " = ";
				} else {
					// Variant's [c_out] takes ownership of the variant value
					r_output << "godot_variant " << C_LOCAL_RET " = ";
				}
			}

			r_output << C_CLASS_NATIVE_FUNCS ".godotsharp_method_bind_call("
					 << CS_PARAM_METHODBIND ", " << (p_icall.is_static ? "IntPtr.Zero" : CS_PARAM_INSTANCE)
					 << ", " << (p_icall.get_arguments_count() ? "(godot_variant**)" C_LOCAL_PTRCALL_ARGS : "null")
					 << ", total_length, out godot_variant_call_error vcall_error);\n";

			r_output << base_indent << "ExceptionUtils.DebugCheckCallError(caller"
					 << ", " << (p_icall.is_static ? "IntPtr.Zero" : CS_PARAM_INSTANCE)
					 << ", " << (p_icall.get_arguments_count() ? "(godot_variant**)" C_LOCAL_PTRCALL_ARGS : "null")
					 << ", total_length, vcall_error);\n";

			if (!ret_void) {
				if (return_type->cname != generator.name_cache.type_Variant) {
					if (return_type->cname == generator.name_cache.enum_Error) {
						r_output << base_indent << C_LOCAL_RET " = VariantUtils.ConvertToInt64(" C_LOCAL_VARARG_RET ");\n";
					} else {
						// TODO: Use something similar to c_in_vararg (see usage above, with error if not implemented)
						CRASH_NOW_MSG("Custom VarArg return type not implemented: " + return_type->name);
						r_output << base_indent << C_LOCAL_RET " = " C_LOCAL_VARARG_RET ";\n";
					}
				}
			}
		} else {
			// MethodBind PtrCall
			r_output << base_indent << C_CLASS_NATIVE_FUNCS ".godotsharp_method_bind_ptrcall("
					 << CS_PARAM_METHODBIND ", " << (p_icall.is_static ? "IntPtr.Zero" : CS_PARAM_INSTANCE)
					 << ", " << (p_icall.get_arguments_count() ? C_LOCAL_PTRCALL_ARGS : "null")
					 << ", " << (!ret_void ? "&" C_LOCAL_RET ");\n" : "null);\n");
		}

		// Return statement

		if (!ret_void) {
			if (return_type->c_out.is_empty()) {
				r_output << base_indent << "return " C_LOCAL_RET ";\n";
			} else {
				r_output << sformat(return_type->c_out, return_type->c_type_out, C_LOCAL_RET,
						return_type->name, String(), String(), base_indent);
			}
		}
	};

	if (p_icall.get_arguments_count()) {
		if (p_icall.is_vararg) {
			String vararg_arg = "arg" + argc_str;
			String real_argc_str = itos(p_icall.get_arguments_count() - 1); // Arguments count without vararg

			p_icall.get_arguments_count();

			r_output << INDENT2 "int vararg_length = " << vararg_arg << ".Length;\n"
					 << INDENT2 "int total_length = " << real_argc_str << " + vararg_length;\n";

			r_output << INDENT2 "Span<godot_variant.movable> varargs_span = vararg_length <= VarArgsSpanThreshold ?\n"
					 << INDENT3 "stackalloc godot_variant.movable[VarArgsSpanThreshold] :\n"
					 << INDENT3 "new godot_variant.movable[vararg_length];\n";

			r_output << INDENT2 "Span<IntPtr> " C_LOCAL_PTRCALL_ARGS "_span = total_length <= VarArgsSpanThreshold ?\n"
					 << INDENT3 "stackalloc IntPtr[VarArgsSpanThreshold] :\n"
					 << INDENT3 "new IntPtr[total_length];\n";

			r_output << INDENT2 "fixed (godot_variant.movable* varargs = &MemoryMarshal.GetReference(varargs_span))\n"
					 << INDENT2 "fixed (IntPtr* " C_LOCAL_PTRCALL_ARGS " = "
								"&MemoryMarshal.GetReference(" C_LOCAL_PTRCALL_ARGS "_span))\n"
					 << OPEN_BLOCK_L2;

			r_output << c_in_statements.as_string();

			r_output << INDENT3 "for (int i = 0; i < vararg_length; i++)\n" OPEN_BLOCK_L3
					 << INDENT4 "varargs[i] = " << vararg_arg << "[i].NativeVar;\n"
					 << INDENT4 C_LOCAL_PTRCALL_ARGS "[" << real_argc_str << " + i] = new IntPtr(&varargs[i]);\n"
					 << CLOSE_BLOCK_L3;

			generate_call_and_return_stmts(INDENT3);

			r_output << CLOSE_BLOCK_L2;
		} else {
			r_output << c_in_statements.as_string();

			r_output << INDENT2 "void** " C_LOCAL_PTRCALL_ARGS " = stackalloc void*["
					 << argc_str << "] { " << c_args_var_content.as_string() << " };\n";

			generate_call_and_return_stmts(INDENT2);
		}
	} else {
		generate_call_and_return_stmts(INDENT2);
	}

	r_output << CLOSE_BLOCK_L1;

	return OK;
}

bool BindingsGenerator::_populate_object_type_interfaces() {
	obj_types.clear();

	LocalVector<StringName> class_list;
	ClassDB::get_class_list(class_list);

	return generator._populate_object_type_interfaces(class_list, obj_types, enum_types);
}

void BindingsGenerator::_populate_builtin_type_interfaces() {
	builtin_types.clear();

	TypeInterface itype;

#define INSERT_STRUCT_TYPE(m_type, m_proxy_name)                                          \
	{                                                                                     \
		itype = TypeInterface::create_value_type(String(#m_type), String(#m_proxy_name)); \
		itype.cs_in_expr = "&%0";                                                         \
		itype.cs_in_expr_is_unsafe = true;                                                \
		builtin_types.insert(itype.cname, itype);                                         \
	}

	INSERT_STRUCT_TYPE(Vector2, Vector2)
	INSERT_STRUCT_TYPE(Vector2i, Vector2I)
	INSERT_STRUCT_TYPE(Rect2, Rect2)
	INSERT_STRUCT_TYPE(Rect2i, Rect2I)
	INSERT_STRUCT_TYPE(Transform2D, Transform2D)
	INSERT_STRUCT_TYPE(Vector3, Vector3)
	INSERT_STRUCT_TYPE(Vector3i, Vector3I)
	INSERT_STRUCT_TYPE(Basis, Basis)
	INSERT_STRUCT_TYPE(Quaternion, Quaternion)
	INSERT_STRUCT_TYPE(Transform3D, Transform3D)
	INSERT_STRUCT_TYPE(AABB, Aabb)
	INSERT_STRUCT_TYPE(Color, Color)
	INSERT_STRUCT_TYPE(Plane, Plane)
	INSERT_STRUCT_TYPE(Vector4, Vector4)
	INSERT_STRUCT_TYPE(Vector4i, Vector4I)
	INSERT_STRUCT_TYPE(Projection, Projection)

#undef INSERT_STRUCT_TYPE

	// bool
	itype = TypeInterface::create_value_type(String("bool"));
	itype.cs_in_expr = "%0.ToGodotBool()";
	itype.cs_out = "%5return %0(%1).ToBool();";
	itype.c_type = "godot_bool";
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.c_type;
	itype.c_arg_in = "&%s";
	itype.c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromBool(%1);\n";
	builtin_types.insert(itype.cname, itype);

	// Integer types
	{
		// C interface for 'uint32_t' is the same as that of enums. Remember to apply
		// any of the changes done here to 'TypeInterface::postsetup_enum_type' as well.
#define INSERT_INT_TYPE(m_name, m_int_struct_name)                                             \
	{                                                                                          \
		itype = TypeInterface::create_value_type(String(m_name));                              \
		if (itype.name != "long" && itype.name != "ulong") {                                   \
			itype.c_in = "%5%0 %1_in = %1;\n";                                                 \
			itype.c_out = "%5return (%0)(%1);\n";                                              \
			itype.c_type = "long";                                                             \
			itype.c_arg_in = "&%s_in";                                                         \
		} else {                                                                               \
			itype.c_arg_in = "&%s";                                                            \
		}                                                                                      \
		itype.c_type_in = itype.name;                                                          \
		itype.c_type_out = itype.name;                                                         \
		itype.c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromInt(%1);\n"; \
		builtin_types.insert(itype.cname, itype);                                              \
	}

		// The expected type for all integers in ptrcall is 'int64_t', so that's what we use for 'c_type'

		INSERT_INT_TYPE("sbyte", "Int8");
		INSERT_INT_TYPE("short", "Int16");
		INSERT_INT_TYPE("int", "Int32");
		INSERT_INT_TYPE("long", "Int64");
		INSERT_INT_TYPE("byte", "UInt8");
		INSERT_INT_TYPE("ushort", "UInt16");
		INSERT_INT_TYPE("uint", "UInt32");
		INSERT_INT_TYPE("ulong", "UInt64");

#undef INSERT_INT_TYPE
	}

	// Floating point types
	{
		// float
		itype = TypeInterface();
		itype.name = "float";
		itype.cname = itype.name;
		itype.proxy_name = "float";
		itype.cs_type = itype.proxy_name;
		{
			// The expected type for 'float' in ptrcall is 'double'
			itype.c_in = "%5%0 %1_in = %1;\n";
			itype.c_out = "%5return (%0)%1;\n";
			itype.c_type = "double";
			itype.c_arg_in = "&%s_in";
		}
		itype.c_type_in = itype.proxy_name;
		itype.c_type_out = itype.proxy_name;
		itype.c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromFloat(%1);\n";
		builtin_types.insert(itype.cname, itype);

		// double
		itype = TypeInterface();
		itype.name = "double";
		itype.cname = itype.name;
		itype.proxy_name = "double";
		itype.cs_type = itype.proxy_name;
		itype.c_type = "double";
		itype.c_arg_in = "&%s";
		itype.c_type_in = itype.proxy_name;
		itype.c_type_out = itype.proxy_name;
		itype.c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromFloat(%1);\n";
		builtin_types.insert(itype.cname, itype);
	}

	// String
	itype = TypeInterface();
	itype.name = "String";
	itype.cname = itype.name;
	itype.proxy_name = "string";
	itype.cs_type = itype.proxy_name;
	itype.c_in = "%5using %0 %1_in = " C_METHOD_MONOSTR_TO_GODOT "(%1);\n";
	itype.c_out = "%5return " C_METHOD_MONOSTR_FROM_GODOT "(%1);\n";
	itype.c_arg_in = "&%s_in";
	itype.c_type = "godot_string";
	itype.c_type_in = itype.cs_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = true;
	itype.c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromString(%1);\n";
	builtin_types.insert(itype.cname, itype);

	// StringName
	itype = TypeInterface();
	itype.name = "StringName";
	itype.cname = itype.name;
	itype.proxy_name = "StringName";
	itype.cs_type = itype.proxy_name;
	itype.cs_in_expr = "(%1)(%0?.NativeValue ?? default)";
	// Cannot pass null StringName to ptrcall
	itype.c_out = "%5return %0.CreateTakingOwnershipOfDisposableValue(%1);\n";
	itype.c_arg_in = "&%s";
	itype.c_type = "godot_string_name";
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.cs_type;
	itype.c_in_vararg = "%5using godot_variant %1_in = VariantUtils.CreateFromStringName(%1);\n";
	itype.c_type_is_disposable_struct = false; // [c_out] takes ownership
	itype.c_ret_needs_default_initialization = true;
	builtin_types.insert(itype.cname, itype);

	// NodePath
	itype = TypeInterface();
	itype.name = "NodePath";
	itype.cname = itype.name;
	itype.proxy_name = "NodePath";
	itype.cs_type = itype.proxy_name;
	itype.cs_in_expr = "(%1)(%0?.NativeValue ?? default)";
	// Cannot pass null NodePath to ptrcall
	itype.c_out = "%5return %0.CreateTakingOwnershipOfDisposableValue(%1);\n";
	itype.c_arg_in = "&%s";
	itype.c_type = "godot_node_path";
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = false; // [c_out] takes ownership
	itype.c_ret_needs_default_initialization = true;
	builtin_types.insert(itype.cname, itype);

	// RID
	itype = TypeInterface();
	itype.name = "RID";
	itype.cname = itype.name;
	itype.proxy_name = "Rid";
	itype.cs_type = itype.proxy_name;
	itype.c_arg_in = "&%s";
	itype.c_type = itype.cs_type;
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.c_type;
	builtin_types.insert(itype.cname, itype);

	// Variant
	itype = TypeInterface();
	itype.name = "Variant";
	itype.cname = itype.name;
	itype.proxy_name = "Variant";
	itype.cs_type = itype.proxy_name;
	itype.c_in = "%5%0 %1_in = (%0)%1.NativeVar;\n";
	itype.c_out = "%5return Variant.CreateTakingOwnershipOfDisposableValue(%1);\n";
	itype.c_arg_in = "&%s_in";
	itype.c_type = "godot_variant";
	itype.c_type_in = itype.cs_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = false; // [c_out] takes ownership
	itype.c_ret_needs_default_initialization = true;
	builtin_types.insert(itype.cname, itype);

	// Callable
	itype = TypeInterface::create_value_type(String("Callable"));
	itype.cs_in_expr = "%0";
	itype.c_in = "%5using %0 %1_in = " C_METHOD_MANAGED_TO_CALLABLE "(in %1);\n";
	itype.c_out = "%5return " C_METHOD_MANAGED_FROM_CALLABLE "(in %1);\n";
	itype.c_arg_in = "&%s_in";
	itype.c_type = "godot_callable";
	itype.c_type_in = "in " + itype.cs_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = true;
	builtin_types.insert(itype.cname, itype);

	// Signal
	itype = TypeInterface();
	itype.name = "Signal";
	itype.cname = itype.name;
	itype.proxy_name = "Signal";
	itype.cs_type = itype.proxy_name;
	itype.cs_in_expr = "%0";
	itype.c_in = "%5using %0 %1_in = " C_METHOD_MANAGED_TO_SIGNAL "(in %1);\n";
	itype.c_out = "%5return " C_METHOD_MANAGED_FROM_SIGNAL "(in %1);\n";
	itype.c_arg_in = "&%s_in";
	itype.c_type = "godot_signal";
	itype.c_type_in = "in " + itype.cs_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = true;
	builtin_types.insert(itype.cname, itype);

	// VarArg (fictitious type to represent variable arguments)
	itype = TypeInterface();
	itype.name = "VarArg";
	itype.cname = itype.name;
	itype.proxy_name = "ReadOnlySpan<Variant>";
	itype.cs_type = "params Variant[]";
	itype.cs_in_expr = "%0";
	// c_type, c_in and c_arg_in are hard-coded in the generator.
	// c_out and c_type_out are not applicable to VarArg.
	itype.c_arg_in = "&%s_in";
	itype.c_type_in = "ReadOnlySpan<Variant>";
	itype.is_span_compatible = true;
	builtin_types.insert(itype.cname, itype);

#define INSERT_ARRAY_FULL(m_name, m_type, m_managed_type, m_proxy_t)                \
	{                                                                               \
		itype = TypeInterface();                                                    \
		itype.name = #m_name;                                                       \
		itype.cname = itype.name;                                                   \
		itype.proxy_name = #m_proxy_t "[]";                                         \
		itype.cs_type = itype.proxy_name;                                           \
		itype.c_in = "%5using %0 %1_in = " C_METHOD_MONOARRAY_TO(m_type) "(%1);\n"; \
		itype.c_out = "%5return " C_METHOD_MONOARRAY_FROM(m_type) "(%1);\n";        \
		itype.c_arg_in = "&%s_in";                                                  \
		itype.c_type = #m_managed_type;                                             \
		itype.c_type_in = "ReadOnlySpan<" #m_proxy_t ">";                           \
		itype.c_type_out = itype.proxy_name;                                        \
		itype.c_type_is_disposable_struct = true;                                   \
		itype.is_span_compatible = true;                                            \
		builtin_types.insert(itype.name, itype);                                    \
	}

#define INSERT_ARRAY(m_type, m_managed_type, m_proxy_t) INSERT_ARRAY_FULL(m_type, m_type, m_managed_type, m_proxy_t)

	INSERT_ARRAY(PackedInt32Array, godot_packed_int32_array, int);
	INSERT_ARRAY(PackedInt64Array, godot_packed_int64_array, long);
	INSERT_ARRAY_FULL(PackedByteArray, PackedByteArray, godot_packed_byte_array, byte);

	INSERT_ARRAY(PackedFloat32Array, godot_packed_float32_array, float);
	INSERT_ARRAY(PackedFloat64Array, godot_packed_float64_array, double);

	INSERT_ARRAY(PackedStringArray, godot_packed_string_array, string);

	INSERT_ARRAY(PackedColorArray, godot_packed_color_array, Color);
	INSERT_ARRAY(PackedVector2Array, godot_packed_vector2_array, Vector2);
	INSERT_ARRAY(PackedVector3Array, godot_packed_vector3_array, Vector3);
	INSERT_ARRAY(PackedVector4Array, godot_packed_vector4_array, Vector4);

#undef INSERT_ARRAY

	// Array
	itype = TypeInterface();
	itype.name = "Array";
	itype.cname = itype.name;
	itype.proxy_name = itype.name;
	itype.type_parameter_count = 1;
	itype.cs_type = BINDINGS_NAMESPACE_COLLECTIONS "." + itype.proxy_name;
	itype.cs_in_expr = "(%1)(%0 ?? new()).NativeValue";
	itype.c_out = "%5return %0.CreateTakingOwnershipOfDisposableValue(%1);\n";
	itype.c_arg_in = "&%s";
	itype.c_type = "godot_array";
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = false; // [c_out] takes ownership
	itype.c_ret_needs_default_initialization = true;
	builtin_types.insert(itype.cname, itype);

	// Array_@generic
	// Reuse Array's itype
	itype.name = "Array_@generic";
	itype.cname = itype.name;
	itype.cs_out = "%5return new %2(%0(%1));";
	// For generic Godot collections, Variant.From<T>/As<T> is slower, so we need this special case
	itype.cs_variant_to_managed = "VariantUtils.ConvertToArray(%0)";
	itype.cs_managed_to_variant = "VariantUtils.CreateFromArray(%0)";
	builtin_types.insert(itype.cname, itype);

	// Dictionary
	itype = TypeInterface();
	itype.name = "Dictionary";
	itype.cname = itype.name;
	itype.proxy_name = itype.name;
	itype.type_parameter_count = 2;
	itype.cs_type = BINDINGS_NAMESPACE_COLLECTIONS "." + itype.proxy_name;
	itype.cs_in_expr = "(%1)(%0 ?? new()).NativeValue";
	itype.c_out = "%5return %0.CreateTakingOwnershipOfDisposableValue(%1);\n";
	itype.c_arg_in = "&%s";
	itype.c_type = "godot_dictionary";
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.cs_type;
	itype.c_type_is_disposable_struct = false; // [c_out] takes ownership
	itype.c_ret_needs_default_initialization = true;
	builtin_types.insert(itype.cname, itype);

	// Dictionary_@generic
	// Reuse Dictionary's itype
	itype.name = "Dictionary_@generic";
	itype.cname = itype.name;
	itype.cs_out = "%5return new %2(%0(%1));";
	// For generic Godot collections, Variant.From<T>/As<T> is slower, so we need this special case
	itype.cs_variant_to_managed = "VariantUtils.ConvertToDictionary(%0)";
	itype.cs_managed_to_variant = "VariantUtils.CreateFromDictionary(%0)";
	builtin_types.insert(itype.cname, itype);

	// void (fictitious type to represent the return type of methods that do not return anything)
	itype = TypeInterface();
	itype.name = "void";
	itype.cname = itype.name;
	itype.proxy_name = itype.name;
	itype.cs_type = itype.proxy_name;
	itype.c_type = itype.proxy_name;
	itype.c_type_in = itype.c_type;
	itype.c_type_out = itype.c_type;
	builtin_types.insert(itype.cname, itype);
}

void BindingsGenerator::_populate_global_constants() {
	int global_constants_count = CoreConstants::get_global_constant_count();

	if (global_constants_count > 0) {
		HashMap<String, DocData::ClassDoc>::Iterator match = EditorHelp::get_doc_data()->class_list.find("@GlobalScope");

		CRASH_COND_MSG(!match, "Could not find '@GlobalScope' in DocData.");

		const DocData::ClassDoc &global_scope_doc = match->value;

		for (int i = 0; i < global_constants_count; i++) {
			String constant_name = CoreConstants::get_global_constant_name(i);

			const DocData::ConstantDoc *const_doc = nullptr;
			for (int j = 0; j < global_scope_doc.constants.size(); j++) {
				const DocData::ConstantDoc &curr_const_doc = global_scope_doc.constants[j];

				if (curr_const_doc.name == constant_name) {
					const_doc = &curr_const_doc;
					break;
				}
			}

			int64_t constant_value = CoreConstants::get_global_constant_value(i);
			StringName enum_name = CoreConstants::get_global_constant_enum(i);

			ConstantInterface iconstant(constant_name, snake_to_pascal_case(constant_name, true), constant_value);
			iconstant.const_doc = const_doc;

			if (enum_name != StringName()) {
				EnumInterface ienum(enum_name, pascal_to_pascal_case(enum_name.operator String()), CoreConstants::is_global_constant_bitfield(i));
				List<EnumInterface>::Element *enum_match = global_enums.find(ienum);
				if (enum_match) {
					enum_match->get().constants.push_back(iconstant);
				} else {
					ienum.constants.push_back(iconstant);
					global_enums.push_back(ienum);
				}
			} else {
				global_constants.push_back(iconstant);
			}
		}

		for (EnumInterface &ienum : global_enums) {
			TypeInterface enum_itype;
			enum_itype.is_enum = true;
			enum_itype.name = ienum.cname.operator String();
			enum_itype.cname = ienum.cname;
			enum_itype.proxy_name = ienum.proxy_name;
			TypeInterface::postsetup_enum_type(enum_itype);
			enum_types.insert(enum_itype.cname, enum_itype);

			int prefix_length = generator._determine_enum_prefix(ienum);

			// HARDCODED: The Error enum have the prefix 'ERR_' for everything except 'OK' and 'FAILED'.
			if (ienum.cname == generator.name_cache.enum_Error) {
				if (prefix_length > 0) { // Just in case it ever changes
					ERR_PRINT("Prefix for enum '" _STR(Error) "' is not empty.");
				}

				prefix_length = 1; // 'ERR_'
			}

			generator._apply_prefix_to_enum_constants(ienum, prefix_length);
		}
	}

	for (int i = 0; i < Variant::VARIANT_MAX; i++) {
		if (i == Variant::OBJECT) {
			continue;
		}

		const Variant::Type type = Variant::Type(i);

		List<StringName> enum_names;
		Variant::get_enums_for_type(type, &enum_names);

		for (const StringName &enum_name : enum_names) {
			TypeInterface enum_itype;
			enum_itype.is_enum = true;
			enum_itype.name = Variant::get_type_name(type) + "." + enum_name;
			enum_itype.cname = enum_itype.name;
			enum_itype.proxy_name = pascal_to_pascal_case(enum_itype.name);
			TypeInterface::postsetup_enum_type(enum_itype);
			enum_types.insert(enum_itype.cname, enum_itype);
		}
	}
}

void BindingsGenerator::_initialize_blacklisted_methods() {
	generator.blacklisted_methods["Object"].push_back("to_string"); // there is already ToString
	generator.blacklisted_methods["Object"].push_back("_to_string"); // override ToString instead
	generator.blacklisted_methods["Object"].push_back("_init"); // never called in C# (TODO: implement it)
}

void BindingsGenerator::_initialize_compat_singletons() {
	generator.compat_singletons.insert("EditorInterface");
}

void BindingsGenerator::_initialize() {
	initialized = false;

	EditorHelp::generate_doc(false);

	enum_types.clear();

	_initialize_blacklisted_methods();

	_initialize_compat_singletons();

	bool obj_type_ok = _populate_object_type_interfaces();
	ERR_FAIL_COND_MSG(!obj_type_ok, "Failed to generate object type interfaces");

	_populate_builtin_type_interfaces();

	_populate_global_constants();

	// Generate internal calls (after populating type interfaces and global constants)

	for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
		const TypeInterface &itype = E.value;
		Error err = _populate_method_icalls_table(itype);
		ERR_FAIL_COND_MSG(err != OK, "Failed to generate icalls table for type: " + itype.name);
	}

	initialized = true;
}

static String generate_all_glue_option = "--generate-mono-glue";

static void handle_cmdline_options(String glue_dir_path) {
	BindingsGenerator bindings_generator;
	bindings_generator.set_log_print_enabled(true);

	if (!bindings_generator.is_initialized()) {
		ERR_PRINT("Failed to initialize the bindings generator");
		return;
	}

	CRASH_COND(glue_dir_path.is_empty());

	if (bindings_generator.generate_cs_api(glue_dir_path.path_join(API_SOLUTION_NAME)) != OK) {
		ERR_PRINT(generate_all_glue_option + ": Failed to generate the C# API.");
	}
}

static void cleanup_and_exit_godot() {
	// Exit once done.
	Main::cleanup(true);
	::exit(0);
}

void BindingsGenerator::handle_cmdline_args(const List<String> &p_cmdline_args) {
	String glue_dir_path;

	const List<String>::Element *elem = p_cmdline_args.front();

	while (elem) {
		if (elem->get() == generate_all_glue_option) {
			const List<String>::Element *path_elem = elem->next();

			if (path_elem) {
				glue_dir_path = path_elem->get();
				elem = elem->next();
			} else {
				ERR_PRINT(generate_all_glue_option + ": No output directory specified (expected path to '{GODOT_ROOT}/modules/mono/glue').");
				// Exit once done with invalid command line arguments.
				cleanup_and_exit_godot();
			}

			break;
		}

		elem = elem->next();
	}

	if (glue_dir_path.length()) {
		if (Engine::get_singleton()->is_editor_hint() ||
				Engine::get_singleton()->is_project_manager_hint()) {
			handle_cmdline_options(glue_dir_path);
		} else {
			// Running from a project folder, which doesn't make sense and crashes.
			ERR_PRINT(generate_all_glue_option + ": Cannot generate Mono glue while running a game project. Change current directory or enable --editor.");
		}
		// Exit once done.
		cleanup_and_exit_godot();
	}
}

#endif // DEBUG_ENABLED
