#include "../bindings_generator_defs.h"
#include "../godotsharp_defs.h"
#include "../utils/naming_utils.h"
#include "../utils/string_utils.h"
#include "runtime_bindings_generator.h"

using TypeInterface = RuntimeBindingsGenerator::TypeInterface;
using ConstantInterface = RuntimeBindingsGenerator::ConstantInterface;
using MethodInterface = RuntimeBindingsGenerator::MethodInterface;
using ArgumentInterface = RuntimeBindingsGenerator::ArgumentInterface;
using PropertyInterface = RuntimeBindingsGenerator::PropertyInterface;
using EnumInterface = RuntimeBindingsGenerator::EnumInterface;
using SignalInterface = RuntimeBindingsGenerator::SignalInterface;
using TypeReference = RuntimeBindingsGenerator::TypeReference;
using InternalCall = RuntimeBindingsGenerator::InternalCall;

// The following properties currently need to be defined with `new` to avoid warnings. We treat
// them as a special case instead of silencing the warnings altogether, to be warned if more
// shadowing appears.
const Vector<String> prop_allowed_inherited_member_hiding = {
	"ArrayMesh.BlendShapeMode",
	"Button.TextDirection",
	"Label.TextDirection",
	"LineEdit.TextDirection",
	"LinkButton.TextDirection",
	"MenuBar.TextDirection",
	"RichTextLabel.TextDirection",
	"TextEdit.TextDirection",
	"FoldableContainer.TextDirection",
	"VisualShaderNodeReroute.PortType",
	// The following instances are uniquely egregious violations, hiding `GetType()` from `object`.
	// Included for the sake of CI, with the understanding that they *deserve* warnings.
	"GltfAccessor.GetType",
	"GltfAccessor.MethodName.GetType",
};

String fix_doc_description(const String &p_bbcode) {
	// This seems to be the correct way to do this. It's the same EditorHelp does.

	return p_bbcode.dedent()
			.remove_chars("\r")
			.strip_edges();
}

const TypeInterface *RuntimeBindingsGenerator::_get_type_or_singleton_or_null(const TypeReference &p_typeref,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	const TypeInterface *itype = _get_type_or_null(p_typeref, builtin_types, obj_types, enum_types);
	if (itype == nullptr) {
		return nullptr;
	}

	if (itype->is_singleton) {
		StringName instance_type_name = itype->name + CS_SINGLETON_INSTANCE_SUFFIX;
		itype = &obj_types.find(instance_type_name)->value;
	}

	return itype;
}

const String RuntimeBindingsGenerator::_get_generic_type_parameters(const TypeInterface &p_itype,
		const List<TypeReference> &p_generic_type_parameters,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	if (p_generic_type_parameters.is_empty()) {
		return "";
	}

	ERR_FAIL_COND_V_MSG(p_itype.type_parameter_count != p_generic_type_parameters.size(), "",
			"Generic type parameter count mismatch for type '" + p_itype.name + "'." +
					" Found " + itos(p_generic_type_parameters.size()) + ", but requires " +
					itos(p_itype.type_parameter_count) + ".");

	int i = 0;
	String params = "<";
	for (const TypeReference &param_type : p_generic_type_parameters) {
		const TypeInterface *param_itype = _get_type_or_singleton_or_null(param_type, builtin_types,
				obj_types, enum_types);
		ERR_FAIL_NULL_V_MSG(param_itype, "", "Parameter type '" + param_type.cname + "' was not found.");

		ERR_FAIL_COND_V_MSG(param_itype->is_singleton, "",
				"Generic type parameter is a singleton: '" + param_itype->name + "'.");

		if (p_itype.api_type == ClassDB::API_CORE) {
			ERR_FAIL_COND_V_MSG(param_itype->api_type == ClassDB::API_EDITOR, "",
					"Generic type parameter '" + param_itype->name + "' has type from the editor API." +
							" Core API cannot have dependencies on the editor API.");
		}

		params += param_itype->cs_type;
		if (i < p_generic_type_parameters.size() - 1) {
			params += ", ";
		}

		i++;
	}
	params += ">";

	return params;
}

void RuntimeBindingsGenerator::_append_text_undeclared(StringBuilder &p_output, const String &p_link_target) {
	p_output.append("'" + p_link_target + "'");
}

void RuntimeBindingsGenerator::_append_text_method(StringBuilder &p_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target, const Vector<String> &p_link_target_parts,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	if (p_link_target_parts[0] == name_cache.type_at_GlobalScope) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			OS::get_singleton()->print("Cannot resolve @GlobalScope method reference in documentation: %s\n", p_link_target.utf8().get_data());
		}

		// TODO Map what we can
		_append_text_undeclared(p_output, p_link_target);
	} else if (!p_target_itype || !p_target_itype->is_object_type) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve method reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from method reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_text_undeclared(p_output, p_link_target);
	} else {
		if (p_target_cname == "_init") {
			// The _init method is not declared in C#, reference the constructor instead
			p_output.append("'new " BINDINGS_NAMESPACE ".");
			p_output.append(p_target_itype->proxy_name);
			p_output.append("()'");
		} else if (p_target_cname == "to_string") {
			// C# uses the built-in object.ToString() method, reference that instead.
			p_output.append("'object.ToString()'");
		} else {
			const MethodInterface *target_imethod = p_target_itype->find_method_by_name(p_target_cname);

			if (target_imethod) {
				p_output.append("'" BINDINGS_NAMESPACE ".");
				p_output.append(p_target_itype->proxy_name);
				p_output.append(".");
				p_output.append(target_imethod->proxy_name);
				p_output.append("(");
				bool first_key = true;
				for (const ArgumentInterface &iarg : target_imethod->arguments) {
					const TypeInterface *arg_type = _get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);

					if (first_key) {
						first_key = false;
					} else {
						p_output.append(", ");
					}
					if (!arg_type) {
						ERR_PRINT("Cannot resolve argument type in documentation: '" + p_link_target + "'.");
						p_output.append(iarg.type.cname);
						continue;
					}
					if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
						p_output.append("Nullable<");
					}
					String arg_cs_type = arg_type->cs_type +
							_get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
									builtin_types, obj_types, enum_types);
					p_output.append(arg_cs_type.replacen("params ", ""));
					if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
						p_output.append(">");
					}
				}
				p_output.append(")'");
			} else {
				if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
					ERR_PRINT("Cannot resolve method reference in documentation: '" + p_link_target + "'.");
				}
				_append_text_undeclared(p_output, p_link_target);
			}
		}
	}
}

void RuntimeBindingsGenerator::_append_xml_undeclared(StringBuilder &p_xml_output, const String &p_link_target) {
	p_xml_output.append("<c>");
	p_xml_output.append(p_link_target);
	p_xml_output.append("</c>");
}

void RuntimeBindingsGenerator::_append_text_member(StringBuilder &p_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target, const Vector<String> &p_link_target_parts,
		const HashMap<StringName, TypeInterface> &builtin_types, const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	if (p_link_target.contains_char('/')) {
		// Properties with '/' (slash) in the name are not declared in C#, so there is nothing to reference.
		_append_text_undeclared(p_output, p_link_target);
	} else if (!p_target_itype || !p_target_itype->is_object_type) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve member reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from member reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_text_undeclared(p_output, p_link_target);
	} else {
		const TypeInterface *current_itype = p_target_itype;
		const PropertyInterface *target_iprop = nullptr;

		while (target_iprop == nullptr && current_itype != nullptr) {
			target_iprop = current_itype->find_property_by_name(p_target_cname);
			if (target_iprop == nullptr) {
				current_itype = _get_type_or_null(TypeReference(current_itype->base_name), builtin_types,
						obj_types, enum_types);
			}
		}

		if (target_iprop) {
			p_output.append("'" BINDINGS_NAMESPACE ".");
			p_output.append(current_itype->proxy_name);
			p_output.append(".");
			p_output.append(target_iprop->proxy_name);
			p_output.append("'");
		} else {
			if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
				ERR_PRINT("Cannot resolve member reference in documentation: '" + p_link_target + "'.");
			}

			_append_text_undeclared(p_output, p_link_target);
		}
	}
}

void RuntimeBindingsGenerator::_append_text_signal(StringBuilder &p_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target, const Vector<String> &p_link_target_parts) {
	if (!p_target_itype || !p_target_itype->is_object_type) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve signal reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from signal reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_text_undeclared(p_output, p_link_target);
	} else {
		const SignalInterface *target_isignal = p_target_itype->find_signal_by_name(p_target_cname);

		if (target_isignal) {
			p_output.append("'" BINDINGS_NAMESPACE ".");
			p_output.append(p_target_itype->proxy_name);
			p_output.append(".");
			p_output.append(target_isignal->proxy_name);
			p_output.append("'");
		} else {
			if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
				ERR_PRINT("Cannot resolve signal reference in documentation: '" + p_link_target + "'.");
			}

			_append_text_undeclared(p_output, p_link_target);
		}
	}
}

void RuntimeBindingsGenerator::_append_text_enum(StringBuilder &p_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target, const Vector<String> &p_link_target_parts,
		const HashMap<StringName, TypeInterface> &enum_types) {
	const StringName search_cname = !p_target_itype ? p_target_cname : StringName(p_target_itype->name + "." + (String)p_target_cname);

	HashMap<StringName, TypeInterface>::ConstIterator enum_match = enum_types.find(search_cname);

	if (!enum_match && search_cname != p_target_cname) {
		enum_match = enum_types.find(p_target_cname);
	}

	if (enum_match) {
		const TypeInterface &target_enum_itype = enum_match->value;

		p_output.append("'" BINDINGS_NAMESPACE ".");
		p_output.append(target_enum_itype.proxy_name); // Includes nesting class if any
		p_output.append("'");
	} else {
		if (p_target_itype == nullptr || !p_target_itype->is_intentionally_ignored(p_target_cname)) {
			ERR_PRINT("Cannot resolve enum reference in documentation: '" + p_link_target + "'.");
		}

		_append_text_undeclared(p_output, p_link_target);
	}
}

void RuntimeBindingsGenerator::_append_text_constant_in_global_scope(StringBuilder &p_output,
		const String &p_target_cname, const String &p_link_target,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums) {
	// Try to find as a global constant
	const ConstantInterface *target_iconst = find_constant_by_name(p_target_cname, global_constants);

	if (target_iconst) {
		// Found global constant
		p_output.append("'" BINDINGS_NAMESPACE "." BINDINGS_GLOBAL_SCOPE_CLASS ".");
		p_output.append(target_iconst->proxy_name);
		p_output.append("'");
	} else {
		// Try to find as global enum constant
		const EnumInterface *target_ienum = nullptr;

		for (const EnumInterface &ienum : global_enums) {
			target_ienum = &ienum;
			target_iconst = find_constant_by_name(p_target_cname, target_ienum->constants);
			if (target_iconst) {
				break;
			}
		}

		if (target_iconst) {
			p_output.append("'" BINDINGS_NAMESPACE ".");
			p_output.append(target_ienum->proxy_name);
			p_output.append(".");
			p_output.append(target_iconst->proxy_name);
			p_output.append("'");
		} else {
			ERR_PRINT("Cannot resolve global constant reference in documentation: '" + p_link_target + "'.");
			_append_text_undeclared(p_output, p_link_target);
		}
	}
}

void RuntimeBindingsGenerator::_append_text_constant(StringBuilder &p_output,
		const TypeInterface *p_target_itype, const StringName &p_target_cname,
		const String &p_link_target, const Vector<String> &p_link_target_parts,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums,
		const HashMap<StringName, TypeInterface> &obj_types) {
	if (p_link_target_parts[0] == name_cache.type_at_GlobalScope) {
		_append_text_constant_in_global_scope(p_output, p_target_cname, p_link_target,
				global_constants, global_enums);
	} else if (!p_target_itype || !p_target_itype->is_object_type) {
		// Search in @GlobalScope as a last resort if no class was specified
		if (p_link_target_parts.size() == 1) {
			_append_text_constant_in_global_scope(p_output, p_target_cname, p_link_target,
					global_constants, global_enums);
			return;
		}

		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve constant reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from constant reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_text_undeclared(p_output, p_link_target);
	} else {
		// Try to find the constant in the current class
		if (p_target_itype->is_singleton_instance) {
			// Constants and enums are declared in the static singleton class.
			p_target_itype = &obj_types[p_target_itype->cname];
		}

		const ConstantInterface *target_iconst = find_constant_by_name(p_target_cname, p_target_itype->constants);

		if (target_iconst) {
			// Found constant in current class
			p_output.append("'" BINDINGS_NAMESPACE ".");
			p_output.append(p_target_itype->proxy_name);
			p_output.append(".");
			p_output.append(target_iconst->proxy_name);
			p_output.append("'");
		} else {
			// Try to find as enum constant in the current class
			const EnumInterface *target_ienum = nullptr;

			for (const EnumInterface &ienum : p_target_itype->enums) {
				target_ienum = &ienum;
				target_iconst = find_constant_by_name(p_target_cname, target_ienum->constants);
				if (target_iconst) {
					break;
				}
			}

			if (target_iconst) {
				p_output.append("'" BINDINGS_NAMESPACE ".");
				p_output.append(p_target_itype->proxy_name);
				p_output.append(".");
				p_output.append(target_ienum->proxy_name);
				p_output.append(".");
				p_output.append(target_iconst->proxy_name);
				p_output.append("'");
			} else if (p_link_target_parts.size() == 1) {
				// Also search in @GlobalScope as a last resort if no class was specified
				_append_text_constant_in_global_scope(p_output, p_target_cname, p_link_target,
						global_constants, global_enums);
			} else {
				if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
					ERR_PRINT("Cannot resolve constant reference in documentation: '" + p_link_target + "'.");
				}

				_append_xml_undeclared(p_output, p_link_target);
			}
		}
	}
}

void RuntimeBindingsGenerator::_append_text_param(StringBuilder &p_output, const String &p_link_target) {
	const String link_target = snake_to_camel_case(p_link_target);
	p_output.append("'" + link_target + "'");
}

String RuntimeBindingsGenerator::bbcode_to_text(const String &p_bbcode, const TypeInterface *p_itype,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	// Based on the version in EditorHelp.

	if (p_bbcode.is_empty()) {
		return String();
	}

	DocTools *doc = EditorHelp::get_doc_data();

	String bbcode = p_bbcode;

	StringBuilder output;

	List<String> tag_stack;
	bool code_tag = false;

	int pos = 0;
	while (pos < bbcode.length()) {
		int brk_pos = bbcode.find_char('[', pos);

		if (brk_pos < 0) {
			brk_pos = bbcode.length();
		}

		if (brk_pos > pos) {
			String text = bbcode.substr(pos, brk_pos - pos);
			if (code_tag || tag_stack.size() > 0) {
				output.append("'" + text + "'");
			} else {
				output.append(text);
			}
		}

		if (brk_pos == bbcode.length()) {
			// Nothing else to add.
			break;
		}

		int brk_end = bbcode.find_char(']', brk_pos + 1);

		if (brk_end == -1) {
			String text = bbcode.substr(brk_pos);
			if (code_tag || tag_stack.size() > 0) {
				output.append("'" + text + "'");
			}

			break;
		}

		String tag = bbcode.substr(brk_pos + 1, brk_end - brk_pos - 1);

		if (tag.begins_with("/")) {
			bool tag_ok = tag_stack.size() && tag_stack.front()->get() == tag.substr(1);

			if (!tag_ok) {
				output.append("]");
				pos = brk_pos + 1;
				continue;
			}

			tag_stack.pop_front();
			pos = brk_end + 1;
			code_tag = false;
		} else if (code_tag) {
			output.append("[");
			pos = brk_pos + 1;
		} else if (tag.begins_with("method ") || tag.begins_with("constructor ") || tag.begins_with("operator ") || tag.begins_with("member ") || tag.begins_with("signal ") || tag.begins_with("enum ") || tag.begins_with("constant ") || tag.begins_with("theme_item ") || tag.begins_with("param ")) {
			const int tag_end = tag.find_char(' ');
			const String link_tag = tag.substr(0, tag_end);
			const String link_target = tag.substr(tag_end + 1).lstrip(" ");

			const Vector<String> link_target_parts = link_target.split(".");

			if (link_target_parts.is_empty() || link_target_parts.size() > 2) {
				ERR_PRINT("Invalid reference format: '" + tag + "'.");

				output.append(tag);

				pos = brk_end + 1;
				continue;
			}

			const TypeInterface *target_itype;
			StringName target_cname;

			if (link_target_parts.size() == 2) {
				target_itype = _get_type_or_null(TypeReference(link_target_parts[0]),
						builtin_types, obj_types, enum_types);
				if (!target_itype) {
					target_itype = _get_type_or_null(TypeReference("_" + link_target_parts[0]),
							builtin_types, obj_types, enum_types);
				}
				target_cname = link_target_parts[1];
			} else {
				target_itype = p_itype;
				target_cname = link_target_parts[0];
			}

			if (!_validate_api_type(target_itype, p_itype)) {
				// If the target member is referenced from a type with a different API level, we can't reference it.
				_append_text_undeclared(output, link_target);
			} else if (link_tag == "method") {
				_append_text_method(output, target_itype, target_cname, link_target, link_target_parts,
						builtin_types, obj_types, enum_types);
			} else if (link_tag == "constructor") {
				// TODO: Support constructors?
				_append_text_undeclared(output, link_target);
			} else if (link_tag == "operator") {
				// TODO: Support operators?
				_append_text_undeclared(output, link_target);
			} else if (link_tag == "member") {
				_append_text_member(output, target_itype, target_cname, link_target, link_target_parts,
						builtin_types, obj_types, enum_types);
			} else if (link_tag == "signal") {
				_append_text_signal(output, target_itype, target_cname, link_target, link_target_parts);
			} else if (link_tag == "enum") {
				_append_text_enum(output, target_itype, target_cname, link_target, link_target_parts, enum_types);
			} else if (link_tag == "constant") {
				_append_text_constant(output, target_itype, target_cname, link_target, link_target_parts,
						global_constants, global_enums, obj_types);
			} else if (link_tag == "param") {
				_append_text_param(output, link_target);
			} else if (link_tag == "theme_item") {
				// We do not declare theme_items in any way in C#, so there is nothing to reference.
				_append_text_undeclared(output, link_target);
			}

			pos = brk_end + 1;
		} else if (doc->class_list.has(tag)) {
			if (tag == "Array" || tag == "Dictionary") {
				output.append("'" BINDINGS_NAMESPACE_COLLECTIONS ".");
				output.append(tag);
				output.append("'");
			} else if (tag == "bool" || tag == "int") {
				output.append(tag);
			} else if (tag == "float") {
				output.append(
#ifdef REAL_T_IS_DOUBLE
						"double"
#else
						"float"
#endif
				);
			} else if (tag == "Variant") {
				output.append("'Godot.Variant'");
			} else if (tag == "String") {
				output.append("string");
			} else if (tag == "Nil") {
				output.append("null");
			} else if (tag.begins_with("@")) {
				// @GlobalScope, @GDScript, etc.
				output.append("'" + tag + "'");
			} else if (tag == "PackedByteArray") {
				output.append("byte[]");
			} else if (tag == "PackedInt32Array") {
				output.append("int[]");
			} else if (tag == "PackedInt64Array") {
				output.append("long[]");
			} else if (tag == "PackedFloat32Array") {
				output.append("float[]");
			} else if (tag == "PackedFloat64Array") {
				output.append("double[]");
			} else if (tag == "PackedStringArray") {
				output.append("string[]");
			} else if (tag == "PackedVector2Array") {
				output.append("'" BINDINGS_NAMESPACE ".Vector2[]'");
			} else if (tag == "PackedVector3Array") {
				output.append("'" BINDINGS_NAMESPACE ".Vector3[]'");
			} else if (tag == "PackedColorArray") {
				output.append("'" BINDINGS_NAMESPACE ".Color[]'");
			} else if (tag == "PackedVector4Array") {
				output.append("'" BINDINGS_NAMESPACE ".Vector4[]'");
			} else {
				const TypeInterface *target_itype = _get_type_or_null(TypeReference(tag),
						builtin_types, obj_types, enum_types);

				if (!target_itype) {
					target_itype = _get_type_or_null(TypeReference("_" + tag),
							builtin_types, obj_types, enum_types);
				}

				if (target_itype) {
					output.append("'" + target_itype->proxy_name + "'");
				} else {
					ERR_PRINT("Cannot resolve type reference in documentation: '" + tag + "'.");
					output.append("'" + tag + "'");
				}
			}

			pos = brk_end + 1;
		} else if (tag == "b") {
			// Bold is not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "i") {
			// Italic is not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "code" || tag.begins_with("code ")) {
			code_tag = true;
			pos = brk_end + 1;
			tag_stack.push_front("code");
		} else if (tag == "kbd") {
			// Keyboard combinations are not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "center") {
			// Center alignment is not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "br") {
			// Break is not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "u") {
			// Underline is not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "s") {
			// Strikethrough is not supported.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "url") {
			int end = bbcode.find_char('[', brk_end);
			if (end == -1) {
				end = bbcode.length();
			}
			String url = bbcode.substr(brk_end + 1, end - brk_end - 1);
			// Not supported. Just append the url.
			output.append(url);

			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag.begins_with("url=")) {
			String url = tag.substr(4);
			// Not supported. Just append the url.
			output.append(url);

			pos = brk_end + 1;
			tag_stack.push_front("url");
		} else if (tag == "img") {
			int end = bbcode.find_char('[', brk_end);
			if (end == -1) {
				end = bbcode.length();
			}
			String image = bbcode.substr(brk_end + 1, end - brk_end - 1);

			// Not supported. Just append the bbcode.
			output.append("[img]");
			output.append(image);
			output.append("[/img]");

			pos = end;
			tag_stack.push_front(tag);
		} else if (tag.begins_with("color=")) {
			// Not supported.
			pos = brk_end + 1;
			tag_stack.push_front("color");
		} else if (tag.begins_with("font=")) {
			// Not supported.
			pos = brk_end + 1;
			tag_stack.push_front("font");
		} else {
			// Ignore unrecognized tag.
			output.append("[");
			pos = brk_pos + 1;
		}
	}

	return output.as_string();
}

Error RuntimeBindingsGenerator::_generate_cs_property(const TypeInterface &p_itype,
		const PropertyInterface &p_iprop, StringBuilder &p_output,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &enum_types,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums) {
	const MethodInterface *setter = p_itype.find_method_by_name(p_iprop.setter);

	// Search it in base types too
	const TypeInterface *current_type = &p_itype;
	while (!setter && current_type->base_name != StringName()) {
		auto base_match = obj_types.find(current_type->base_name);
		ERR_FAIL_COND_V_MSG(!base_match, ERR_BUG, "Type not found '" + current_type->base_name + "'. Inherited by '" + current_type->name + "'.");
		current_type = &base_match->value;
		setter = current_type->find_method_by_name(p_iprop.setter);
	}

	const MethodInterface *getter = p_itype.find_method_by_name(p_iprop.getter);

	// Search it in base types too
	current_type = &p_itype;
	while (!getter && current_type->base_name != StringName()) {
		auto base_match = obj_types.find(current_type->base_name);
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

	const TypeInterface *prop_itype = _get_type_or_singleton_or_null(proptype_name,
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
		String xml_summary = bbcode_to_xml(fix_doc_description(p_iprop.prop_doc->description), &p_itype,
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
		p_output.append(bbcode_to_text(p_iprop.deprecation_message, &p_itype, global_constants, global_enums,
				builtin_types, obj_types, enum_types));
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
			_get_generic_type_parameters(*prop_itype, proptype_name.generic_type_parameters,
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
			if (idx_arg.type.cname != name_cache.type_int) {
				// Assume the index parameter is an enum
				const TypeInterface *idx_arg_type = _get_type_or_null(idx_arg.type, builtin_types,
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
			if (idx_arg.type.cname != name_cache.type_int) {
				// Assume the index parameter is an enum
				const TypeInterface *idx_arg_type = _get_type_or_null(idx_arg.type, builtin_types,
						obj_types, enum_types);
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

bool RuntimeBindingsGenerator::_arg_default_value_is_assignable_to_type(const Variant &p_val,
		const TypeInterface &p_arg_type) {
	if (p_arg_type.name == name_cache.type_Variant) {
		// Variant can take anything
		return true;
	}

	switch (p_val.get_type()) {
		case Variant::NIL:
			return p_arg_type.is_object_type ||
					name_cache.is_nullable_type(p_arg_type.name);
		case Variant::BOOL:
			return p_arg_type.name == name_cache.type_bool;
		case Variant::INT:
			return p_arg_type.name == name_cache.type_sbyte ||
					p_arg_type.name == name_cache.type_short ||
					p_arg_type.name == name_cache.type_int ||
					p_arg_type.name == name_cache.type_byte ||
					p_arg_type.name == name_cache.type_ushort ||
					p_arg_type.name == name_cache.type_uint ||
					p_arg_type.name == name_cache.type_long ||
					p_arg_type.name == name_cache.type_ulong ||
					p_arg_type.name == name_cache.type_float ||
					p_arg_type.name == name_cache.type_double ||
					p_arg_type.is_enum;
		case Variant::FLOAT:
			return p_arg_type.name == name_cache.type_float ||
					p_arg_type.name == name_cache.type_double;
		case Variant::STRING:
		case Variant::STRING_NAME:
			return p_arg_type.name == name_cache.type_String ||
					p_arg_type.name == name_cache.type_StringName ||
					p_arg_type.name == name_cache.type_NodePath;
		case Variant::NODE_PATH:
			return p_arg_type.name == name_cache.type_NodePath;
		case Variant::TRANSFORM2D:
		case Variant::TRANSFORM3D:
		case Variant::BASIS:
		case Variant::QUATERNION:
		case Variant::PLANE:
		case Variant::AABB:
		case Variant::COLOR:
		case Variant::VECTOR2:
		case Variant::RECT2:
		case Variant::VECTOR3:
		case Variant::VECTOR4:
		case Variant::PROJECTION:
		case Variant::RID:
		case Variant::PACKED_BYTE_ARRAY:
		case Variant::PACKED_INT32_ARRAY:
		case Variant::PACKED_INT64_ARRAY:
		case Variant::PACKED_FLOAT32_ARRAY:
		case Variant::PACKED_FLOAT64_ARRAY:
		case Variant::PACKED_STRING_ARRAY:
		case Variant::PACKED_VECTOR2_ARRAY:
		case Variant::PACKED_VECTOR3_ARRAY:
		case Variant::PACKED_VECTOR4_ARRAY:
		case Variant::PACKED_COLOR_ARRAY:
		case Variant::CALLABLE:
		case Variant::SIGNAL:
			return p_arg_type.name == Variant::get_type_name(p_val.get_type());
		case Variant::ARRAY:
			return p_arg_type.name == Variant::get_type_name(p_val.get_type()) || p_arg_type.cname == name_cache.type_Array_generic;
		case Variant::DICTIONARY:
			return p_arg_type.name == Variant::get_type_name(p_val.get_type()) || p_arg_type.cname == name_cache.type_Dictionary_generic;
		case Variant::OBJECT:
			return p_arg_type.is_object_type;
		case Variant::VECTOR2I:
			return p_arg_type.name == name_cache.type_Vector2 ||
					p_arg_type.name == Variant::get_type_name(p_val.get_type());
		case Variant::RECT2I:
			return p_arg_type.name == name_cache.type_Rect2 ||
					p_arg_type.name == Variant::get_type_name(p_val.get_type());
		case Variant::VECTOR3I:
			return p_arg_type.name == name_cache.type_Vector3 ||
					p_arg_type.name == Variant::get_type_name(p_val.get_type());
		case Variant::VECTOR4I:
			return p_arg_type.name == name_cache.type_Vector4 ||
					p_arg_type.name == Variant::get_type_name(p_val.get_type());
		case Variant::VARIANT_MAX:
			CRASH_NOW_MSG("Unexpected Variant type: " + itos(p_val.get_type()));
			break;
	}

	return false;
}

Error RuntimeBindingsGenerator::_generate_cs_method(const TypeInterface &p_itype, const MethodInterface &p_imethod,
		int &p_method_bind_count, StringBuilder &p_output, bool p_use_span,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types,
		const HashMap<const MethodInterface *, const InternalCall *> &method_icalls_map) {
	const TypeInterface *return_type = _get_type_or_singleton_or_null(p_imethod.return_type,
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
				const TypeInterface *arg_type = _get_type_or_singleton_or_null(iarg.type,
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
		const TypeInterface *arg_type = _get_type_or_singleton_or_null(iarg.type,
				builtin_types, obj_types, enum_types);
		ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

		ERR_FAIL_COND_V_MSG(arg_type->is_singleton, ERR_BUG,
				"Argument type is a singleton: '" + iarg.name + "' of method '" + p_itype.name + "." + p_imethod.name + "'.");

		if (p_itype.api_type == ClassDB::API_CORE) {
			ERR_FAIL_COND_V_MSG(arg_type->api_type == ClassDB::API_EDITOR, ERR_BUG,
					"Argument '" + iarg.name + "' of method '" + p_itype.name + "." + p_imethod.name + "' has type '" +
							arg_type->name + "' from the editor API. Core API cannot have dependencies on the editor API.");
		}

		if (iarg.default_argument.size()) {
			CRASH_COND_MSG(!_arg_default_value_is_assignable_to_type(iarg.def_param_value, *arg_type),
					"Invalid default value for parameter '" + iarg.name + "' of method '" + p_itype.name + "." + p_imethod.name + "'.");
		}

		String arg_cs_type = arg_type->cs_type +
				_get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
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
			String xml_summary = bbcode_to_xml(fix_doc_description(p_imethod.method_doc->description), &p_itype,
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

		if (default_args_doc.get_string_length()) {
			p_output.append(default_args_doc.as_string());
		}

		if (p_imethod.is_deprecated) {
			p_output.append(MEMBER_BEGIN "[Obsolete(\"");
			p_output.append(bbcode_to_text(p_imethod.deprecation_message, &p_itype,
					global_constants, global_enums, builtin_types, obj_types, enum_types));
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
				_get_generic_type_parameters(*return_type, p_imethod.return_type.generic_type_parameters,
						builtin_types, obj_types, enum_types);

		p_output.append(return_cs_type + " ");
		p_output.append(p_imethod.proxy_name + "(");
		p_output.append(arguments_sig + ")\n" OPEN_BLOCK_L1);

		if (p_imethod.is_virtual) {
			// Godot virtual method must be overridden, therefore we return a default value by default.

			if (return_type->cname == name_cache.type_void) {
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

		if (return_type->cname == name_cache.type_void) {
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

Error RuntimeBindingsGenerator::_generate_cs_signal(const TypeInterface &p_itype, const SignalInterface &p_isignal,
		StringBuilder &p_output, const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	String arguments_sig;

	// Retrieve information from the arguments
	const ArgumentInterface &first = p_isignal.arguments.front()->get();
	for (const ArgumentInterface &iarg : p_isignal.arguments) {
		const TypeInterface *arg_type = _get_type_or_singleton_or_null(iarg.type, builtin_types, obj_types, enum_types);
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
				_get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
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
				p_output.append(bbcode_to_text(p_isignal.deprecation_message, &p_itype, global_constants, global_enums,
						builtin_types, obj_types, enum_types));
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
				p_output.append(bbcode_to_text(p_isignal.deprecation_message, &p_itype, global_constants, global_enums,
						builtin_types, obj_types, enum_types));
				p_output.append("\")]");
			}
			p_output << MEMBER_BEGIN "private static void " << p_isignal.proxy_name << "Trampoline"
					 << "(object delegateObj, NativeVariantPtrArgs args, out godot_variant ret)\n"
					 << INDENT1 "{\n"
					 << INDENT2 "Callable.ThrowIfArgCountMismatch(args, " << itos(p_isignal.arguments.size()) << ");\n"
					 << INDENT2 "((" << delegate_name << ")delegateObj)(";

			int idx = 0;
			for (const ArgumentInterface &iarg : p_isignal.arguments) {
				const TypeInterface *arg_type = _get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);
				ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

				if (idx != 0) {
					p_output << ", ";
				}

				if (arg_type->cname == name_cache.type_Array_generic || arg_type->cname == name_cache.type_Dictionary_generic) {
					String arg_cs_type = arg_type->cs_type +
							_get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
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
			String xml_summary = bbcode_to_xml(fix_doc_description(p_isignal.method_doc->description), &p_itype,
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
			p_output.append(bbcode_to_text(p_isignal.deprecation_message, &p_itype,
					global_constants, global_enums, builtin_types, obj_types, enum_types));
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
				p_output.append(bbcode_to_text(p_isignal.deprecation_message, &p_itype, global_constants,
						global_enums, builtin_types, obj_types, enum_types));
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
					const TypeInterface *arg_type = _get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);
					ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

					if (idx != 0) {
						p_output << ", ";
						cs_emitsignal_params << ", ";
					}

					String arg_cs_type = arg_type->cs_type +
							_get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters, builtin_types,
									obj_types, enum_types);

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

Error RuntimeBindingsGenerator::_generate_cs_native_calls(const InternalCall &p_icall, StringBuilder &r_output,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	bool ret_void = p_icall.return_type.cname == name_cache.type_void;

	const TypeInterface *return_type = _get_type_or_null(p_icall.return_type, builtin_types, obj_types, enum_types);
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
		const TypeInterface *arg_type = _get_type_or_null(arg_type_ref, builtin_types, obj_types, enum_types);
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

	if (!ret_void && (!p_icall.is_vararg || return_type->cname != name_cache.type_Variant)) {
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
				if (return_type->cname != name_cache.type_Variant) {
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
				if (return_type->cname != name_cache.type_Variant) {
					if (return_type->cname == name_cache.enum_Error) {
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

void RuntimeBindingsGenerator::_append_xml_method(StringBuilder &p_xml_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target,
		const Vector<String> &p_link_target_parts, const TypeInterface *p_source_itype,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	if (p_link_target_parts[0] == name_cache.type_at_GlobalScope) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			OS::get_singleton()->print("Cannot resolve @GlobalScope method reference in documentation: %s\n", p_link_target.utf8().get_data());
		}

		// TODO Map what we can
		_append_xml_undeclared(p_xml_output, p_link_target);
	} else if (!p_target_itype || !p_target_itype->is_object_type) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve method reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from method reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_xml_undeclared(p_xml_output, p_link_target);
	} else {
		if (p_target_cname == "_init") {
			// The _init method is not declared in C#, reference the constructor instead.
			p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
			p_xml_output.append(p_target_itype->proxy_name);
			p_xml_output.append(".");
			p_xml_output.append(p_target_itype->proxy_name);
			p_xml_output.append("()\"/>");
		} else if (p_target_cname == "to_string") {
			// C# uses the built-in object.ToString() method, reference that instead.
			p_xml_output.append("<see cref=\"object.ToString()\"/>");
		} else {
			const MethodInterface *target_imethod = p_target_itype->find_method_by_name(p_target_cname);

			if (target_imethod) {
				const String method_name = p_target_itype->proxy_name + "." + target_imethod->proxy_name;
				if (!_validate_api_type(p_target_itype, p_source_itype)) {
					_append_xml_undeclared(p_xml_output, method_name);
				} else {
					p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
					p_xml_output.append(method_name);
					p_xml_output.append("(");
					bool first_key = true;
					for (const ArgumentInterface &iarg : target_imethod->arguments) {
						const TypeInterface *arg_type = _get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);

						if (first_key) {
							first_key = false;
						} else {
							p_xml_output.append(", ");
						}
						if (!arg_type) {
							ERR_PRINT("Cannot resolve argument type in documentation: '" + p_link_target + "'.");
							p_xml_output.append(iarg.type.cname);
							continue;
						}
						if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
							p_xml_output.append("Nullable{");
						}
						String arg_cs_type = arg_type->cs_type + _get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters, builtin_types, obj_types, enum_types);
						p_xml_output.append(arg_cs_type.replacen("<", "{").replacen(">", "}").replacen("params ", ""));
						if (iarg.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
							p_xml_output.append("}");
						}
					}
					p_xml_output.append(")\"/>");
				}
			} else {
				if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
					ERR_PRINT("Cannot resolve method reference in documentation: '" + p_link_target + "'.");
				}

				_append_xml_undeclared(p_xml_output, p_link_target);
			}
		}
	}
}

// FIXME: There are some members that hide other inherited members.
// - In the case of both members being the same kind, the new one must be declared
// explicitly as 'new' to avoid the warning (and we must print a message about it).
// - In the case of both members being of a different kind, then the new one must
// be renamed to avoid the name collision (and we must print a warning about it).
// - Csc warning e.g.:
// ObjectType/LineEdit.cs(140,38): warning CS0108: 'LineEdit.FocusMode' hides inherited member 'Control.FocusMode'. Use the new keyword if hiding was intended.
Error RuntimeBindingsGenerator::_generate_cs_type(const TypeInterface &itype, const String &p_output_file,
		const HashMap<StringName, TypeInterface> &obj_types,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &enum_types,
		const HashMap<const MethodInterface *, const InternalCall *> &method_icalls_map) {
	CRASH_COND(!itype.is_object_type);

	bool is_derived_type = itype.base_name != StringName();

	if (!is_derived_type) {
		// Some GodotObject assertions
		CRASH_COND(itype.cname != name_cache.type_Object);
		CRASH_COND(!itype.is_instantiable);
		CRASH_COND(itype.api_type != ClassDB::API_CORE);
		CRASH_COND(itype.is_ref_counted);
		CRASH_COND(itype.is_singleton);
	}

	_log("Generating %s.cs...\n", itype.proxy_name.utf8().get_data());

	StringBuilder output;

	output.append("namespace " BINDINGS_NAMESPACE ";\n\n");

	output.append("using System;\n"); // IntPtr
	output.append("using System.ComponentModel;\n"); // EditorBrowsable
	output.append("using System.Diagnostics;\n"); // DebuggerBrowsable
	output.append("using Godot.NativeInterop;\n");

	output.append("\n#nullable disable\n");

	const DocData::ClassDoc *class_doc = itype.class_doc;

	if (class_doc && class_doc->description.size()) {
		String xml_summary = bbcode_to_xml(fix_doc_description(class_doc->description), &itype,
				builtin_types, obj_types, enum_types, global_constants, global_enums);
		Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

		if (summary_lines.size()) {
			output.append("/// <summary>\n");

			for (int i = 0; i < summary_lines.size(); i++) {
				output.append("/// ");
				output.append(summary_lines[i]);
				output.append("\n");
			}

			output.append("/// </summary>\n");
		}
	}

	if (itype.is_deprecated) {
		output.append("[Obsolete(\"");
		output.append(bbcode_to_text(itype.deprecation_message, &itype, global_constants, global_enums,
				builtin_types, obj_types, enum_types));
		output.append("\")]\n");
	}

	// We generate a `GodotClassName` attribute if the engine class name is not the same as the
	// generated C# class name. This allows introspection code to find the name associated with
	// the class. If the attribute is not present, the C# class name can be used instead.
	if (itype.name != itype.proxy_name) {
		output << "[GodotClassName(\"" << itype.name << "\")]\n";
	}

	output.append("public ");
	if (itype.is_singleton) {
		output.append("static partial class ");
	} else {
		// Even if the class is not instantiable, we can't declare it abstract because
		// the engine can still instantiate them and return them via the scripting API.
		// Example: `SceneTreeTimer` returned from `SceneTree.create_timer`.
		// See the reverted commit: ef5672d3f94a7321ed779c922088bb72adbb1521
		output.append("partial class ");
	}
	output.append(itype.proxy_name);

	if (is_derived_type && !itype.is_singleton) {
		if (obj_types.has(itype.base_name)) {
			TypeInterface base_type = obj_types[itype.base_name];
			output.append(" : ");
			output.append(base_type.proxy_name);
			if (base_type.is_singleton) {
				// If the type is a singleton, use the instance type.
				output.append(CS_SINGLETON_INSTANCE_SUFFIX);
			}
		} else {
			ERR_PRINT("Base type '" + itype.base_name.operator String() + "' does not exist, for class '" + itype.name + "'.");
			return ERR_INVALID_DATA;
		}
	}

	output.append("\n{");

	// Add constants

	for (const ConstantInterface &iconstant : itype.constants) {
		if (iconstant.const_doc && iconstant.const_doc->description.size()) {
			String xml_summary = bbcode_to_xml(fix_doc_description(iconstant.const_doc->description), &itype,
					builtin_types, obj_types, enum_types, global_constants, global_enums);
			Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

			if (summary_lines.size()) {
				output.append(MEMBER_BEGIN "/// <summary>\n");

				for (int i = 0; i < summary_lines.size(); i++) {
					output.append(INDENT1 "/// ");
					output.append(summary_lines[i]);
					output.append("\n");
				}

				output.append(INDENT1 "/// </summary>");
			}
		}

		if (iconstant.is_deprecated) {
			output.append(MEMBER_BEGIN "[Obsolete(\"");
			output.append(bbcode_to_text(iconstant.deprecation_message, &itype, global_constants, global_enums,
					builtin_types, obj_types, enum_types));
			output.append("\")]");
		}

		output.append(MEMBER_BEGIN "public const long ");
		output.append(iconstant.proxy_name);
		output.append(" = ");
		output.append(itos(iconstant.value));
		output.append(";");
	}

	if (itype.constants.size()) {
		output.append("\n");
	}

	// Add enums

	for (const EnumInterface &ienum : itype.enums) {
		ERR_FAIL_COND_V(ienum.constants.is_empty(), ERR_BUG);

		if (ienum.is_flags) {
			output.append(MEMBER_BEGIN "[System.Flags]");
		}

		output.append(MEMBER_BEGIN "public enum ");
		output.append(ienum.proxy_name);
		output.append(" : long");
		output.append(MEMBER_BEGIN OPEN_BLOCK);

		const ConstantInterface &last = ienum.constants.back()->get();
		for (const ConstantInterface &iconstant : ienum.constants) {
			if (iconstant.const_doc && iconstant.const_doc->description.size()) {
				String xml_summary = bbcode_to_xml(fix_doc_description(iconstant.const_doc->description), &itype,
						builtin_types, obj_types, enum_types, global_constants, global_enums);
				Vector<String> summary_lines = xml_summary.length() ? xml_summary.split("\n") : Vector<String>();

				if (summary_lines.size()) {
					output.append(INDENT2 "/// <summary>\n");

					for (int i = 0; i < summary_lines.size(); i++) {
						output.append(INDENT2 "/// ");
						output.append(summary_lines[i]);
						output.append("\n");
					}

					output.append(INDENT2 "/// </summary>\n");
				}
			}

			if (iconstant.is_deprecated) {
				output.append(INDENT2 "[Obsolete(\"");
				output.append(bbcode_to_text(iconstant.deprecation_message, &itype, global_constants,
						global_enums, builtin_types, obj_types, enum_types));
				output.append("\")]\n");
			}

			output.append(INDENT2);
			output.append(iconstant.proxy_name);
			output.append(" = ");
			output.append(itos(iconstant.value));
			output.append(&iconstant != &last ? ",\n" : "\n");
		}

		output.append(INDENT1 CLOSE_BLOCK);
	}

	// Add properties

	for (const PropertyInterface &iprop : itype.properties) {
		Error prop_err = _generate_cs_property(itype, iprop, output, obj_types,
				builtin_types, enum_types, global_constants, global_enums);
		ERR_FAIL_COND_V_MSG(prop_err != OK, prop_err,
				"Failed to generate property '" + iprop.cname.operator String() +
						"' for class '" + itype.name + "'.");
	}

	// Add native name static field and cached type.

	if (is_derived_type && !itype.is_singleton) {
		output << MEMBER_BEGIN "private static readonly System.Type CachedType = typeof(" << itype.proxy_name << ");\n";
	}

	output.append(MEMBER_BEGIN "private static readonly StringName " BINDINGS_NATIVE_NAME_FIELD " = \"");
	output.append(itype.name);
	output.append("\";\n");

	if (itype.is_singleton || itype.is_compat_singleton) {
		// Add the Singleton static property.

		String instance_type_name;

		if (itype.is_singleton) {
			StringName instance_name = itype.name + CS_SINGLETON_INSTANCE_SUFFIX;
			instance_type_name = obj_types.has(instance_name)
					? obj_types[instance_name].proxy_name
					: "GodotObject";
		} else {
			instance_type_name = itype.proxy_name;
		}

		output.append(MEMBER_BEGIN "private static " + instance_type_name + " singleton;\n");

		output << MEMBER_BEGIN "public static " + instance_type_name + " " CS_PROPERTY_SINGLETON " =>\n"
			   << INDENT2 "singleton \?\?= (" + instance_type_name + ")"
			   << C_METHOD_ENGINE_GET_SINGLETON "(\"" << itype.name << "\");\n";
	}

	if (!itype.is_singleton) {
		// IMPORTANT: We also generate the static fields for GodotObject instead of declaring
		// them manually in the `GodotObject.base.cs` partial class declaration, because they're
		// required by other static fields in this generated partial class declaration.
		// Static fields are initialized in order of declaration, but when they're in different
		// partial class declarations then it becomes harder to tell (Rider warns about this).

		if (itype.is_instantiable) {
			// Add native constructor static field

			output << MEMBER_BEGIN << "[DebuggerBrowsable(DebuggerBrowsableState.Never)]\n"
				   << INDENT1 "private static readonly unsafe delegate* unmanaged<godot_bool, IntPtr> "
				   << CS_STATIC_FIELD_NATIVE_CTOR " = " ICALL_CLASSDB_GET_CONSTRUCTOR
				   << "(" BINDINGS_NATIVE_NAME_FIELD ");\n";
		}

		if (is_derived_type) {
			// Add default constructor
			if (itype.is_instantiable) {
				output << MEMBER_BEGIN "public " << itype.proxy_name << "() : this("
					   << (itype.memory_own ? "true" : "false") << ")\n" OPEN_BLOCK_L1
					   << INDENT2 "unsafe\n" INDENT2 OPEN_BLOCK
					   << INDENT3 "ConstructAndInitialize(" CS_STATIC_FIELD_NATIVE_CTOR ", "
					   << BINDINGS_NATIVE_NAME_FIELD ", CachedType, refCounted: "
					   << (itype.is_ref_counted ? "true" : "false") << ");\n"
					   << CLOSE_BLOCK_L2 CLOSE_BLOCK_L1;
			} else {
				// Hide the constructor
				output << MEMBER_BEGIN "internal " << itype.proxy_name << "() : this("
					   << (itype.memory_own ? "true" : "false") << ")\n" OPEN_BLOCK_L1
					   << INDENT2 "unsafe\n" INDENT2 OPEN_BLOCK
					   << INDENT3 "ConstructAndInitialize(null, "
					   << BINDINGS_NATIVE_NAME_FIELD ", CachedType, refCounted: "
					   << (itype.is_ref_counted ? "true" : "false") << ");\n"
					   << CLOSE_BLOCK_L2 CLOSE_BLOCK_L1;
			}

			output << MEMBER_BEGIN "internal " << itype.proxy_name << "(IntPtr " CS_PARAM_INSTANCE ") : this("
				   << (itype.memory_own ? "true" : "false") << ")\n" OPEN_BLOCK_L1
				   << INDENT2 "NativePtr = " CS_PARAM_INSTANCE ";\n"
				   << INDENT2 "unsafe\n" INDENT2 OPEN_BLOCK
				   << INDENT3 "ConstructAndInitialize(null, "
				   << BINDINGS_NATIVE_NAME_FIELD ", CachedType, refCounted: "
				   << (itype.is_ref_counted ? "true" : "false") << ");\n"
				   << CLOSE_BLOCK_L2 CLOSE_BLOCK_L1;

			// Add.. em.. trick constructor. Sort of.
			output.append(MEMBER_BEGIN "public ");
			output.append(itype.proxy_name);
			output.append("(bool " CS_PARAM_MEMORYOWN ") : base(" CS_PARAM_MEMORYOWN ") { }\n");
		}
	}

	// Methods

	int method_bind_count = 0;
	for (const MethodInterface &imethod : itype.methods) {
		Error method_err = _generate_cs_method(itype, imethod, method_bind_count, output, false, global_constants,
				global_enums, builtin_types, obj_types, enum_types, method_icalls_map);
		ERR_FAIL_COND_V_MSG(method_err != OK, method_err,
				"Failed to generate method '" + imethod.name + "' for class '" + itype.name + "'.");
		if (imethod.is_internal) {
			// No need to generate span overloads for internal methods.
			continue;
		}

		method_err = _generate_cs_method(itype, imethod, method_bind_count, output, true, global_constants,
				global_enums, builtin_types, obj_types, enum_types, method_icalls_map);
		ERR_FAIL_COND_V_MSG(method_err != OK, method_err,
				"Failed to generate span overload method '" + imethod.name + "' for class '" + itype.name + "'.");
	}

	// Signals

	for (const SignalInterface &isignal : itype.signals_) {
		Error method_err = _generate_cs_signal(itype, isignal, output, global_constants, global_enums,
				builtin_types, obj_types, enum_types);
		ERR_FAIL_COND_V_MSG(method_err != OK, method_err,
				"Failed to generate signal '" + isignal.name + "' for class '" + itype.name + "'.");
	}

	// Script members look-up

	if (!itype.is_singleton && (is_derived_type || itype.has_virtual_methods)) {
		// Generate method names cache fields

		for (const MethodInterface &imethod : itype.methods) {
			if (!imethod.is_virtual) {
				continue;
			}

			output << MEMBER_BEGIN "// ReSharper disable once InconsistentNaming\n"
				   << INDENT1 "[DebuggerBrowsable(DebuggerBrowsableState.Never)]\n"
				   << INDENT1 "private static readonly StringName "
				   << CS_STATIC_FIELD_METHOD_PROXY_NAME_PREFIX << imethod.name
				   << " = \"" << imethod.proxy_name << "\";\n";
		}

		// Generate signal names cache fields

		for (const SignalInterface &isignal : itype.signals_) {
			output << MEMBER_BEGIN "// ReSharper disable once InconsistentNaming\n"
				   << INDENT1 "[DebuggerBrowsable(DebuggerBrowsableState.Never)]\n"
				   << INDENT1 "private static readonly StringName "
				   << CS_STATIC_FIELD_SIGNAL_PROXY_NAME_PREFIX << isignal.name
				   << " = \"" << isignal.proxy_name << "\";\n";
		}

		// TODO: Only generate HasGodotClassMethod and InvokeGodotClassMethod if there's any method

		// Generate InvokeGodotClassMethod

		output << MEMBER_BEGIN "/// <summary>\n"
			   << INDENT1 "/// Invokes the method with the given name, using the given arguments.\n"
			   << INDENT1 "/// This method is used by Godot to invoke methods from the engine side.\n"
			   << INDENT1 "/// Do not call or override this method.\n"
			   << INDENT1 "/// </summary>\n"
			   << INDENT1 "/// <param name=\"method\">Name of the method to invoke.</param>\n"
			   << INDENT1 "/// <param name=\"args\">Arguments to use with the invoked method.</param>\n"
			   << INDENT1 "/// <param name=\"ret\">Value returned by the invoked method.</param>\n";

		// Avoid raising diagnostics because of calls to obsolete methods.
		output << "#pragma warning disable CS0618 // Member is obsolete\n";

		output << INDENT1 "public " << (is_derived_type ? "override" : "virtual")
			   << " bool " CS_METHOD_INVOKE_GODOT_CLASS_METHOD "(in godot_string_name method, "
			   << "NativeVariantPtrArgs args, out godot_variant ret)\n"
			   << INDENT1 "{\n";

		for (const MethodInterface &imethod : itype.methods) {
			if (!imethod.is_virtual) {
				continue;
			}

			// We also call HasGodotClassMethod to ensure the method is overridden and avoid calling
			// the stub implementation. This solution adds some extra overhead to calls, but it's
			// much simpler than other solutions. This won't be a problem once we move to function
			// pointers of generated wrappers for each method, as lookup will only happen once.

			// We check both native names (snake_case) and proxy names (PascalCase)
			output << INDENT2 "if ((method == " << CS_STATIC_FIELD_METHOD_PROXY_NAME_PREFIX << imethod.name
				   << " || method == MethodName." << imethod.proxy_name
				   << ") && args.Count == " << itos(imethod.arguments.size())
				   << " && " << CS_METHOD_HAS_GODOT_CLASS_METHOD << "((godot_string_name)"
				   << CS_STATIC_FIELD_METHOD_PROXY_NAME_PREFIX << imethod.name << ".NativeValue))\n"
				   << INDENT2 "{\n";

			if (imethod.return_type.cname != name_cache.type_void) {
				output << INDENT3 "var callRet = ";
			} else {
				output << INDENT3;
			}

			output << imethod.proxy_name << "(";

			int i = 0;
			for (List<ArgumentInterface>::ConstIterator itr = imethod.arguments.begin(); itr != imethod.arguments.end(); ++itr, ++i) {
				const ArgumentInterface &iarg = *itr;

				const TypeInterface *arg_type = _get_type_or_null(iarg.type, builtin_types, obj_types, enum_types);
				ERR_FAIL_NULL_V_MSG(arg_type, ERR_BUG, "Argument type '" + iarg.type.cname + "' was not found.");

				if (i != 0) {
					output << ", ";
				}

				if (arg_type->cname == name_cache.type_Array_generic || arg_type->cname == name_cache.type_Dictionary_generic) {
					String arg_cs_type = arg_type->cs_type +
							_get_generic_type_parameters(*arg_type, iarg.type.generic_type_parameters,
									builtin_types, obj_types, enum_types);

					output << "new " << arg_cs_type << "(" << sformat(arg_type->cs_variant_to_managed, "args[" + itos(i) + "]", arg_type->cs_type, arg_type->name) << ")";
				} else {
					output << sformat(arg_type->cs_variant_to_managed,
							"args[" + itos(i) + "]", arg_type->cs_type, arg_type->name);
				}
			}

			output << ");\n";

			if (imethod.return_type.cname != name_cache.type_void) {
				const TypeInterface *return_type = _get_type_or_null(imethod.return_type, builtin_types,
						obj_types, enum_types);
				ERR_FAIL_NULL_V_MSG(return_type, ERR_BUG, "Return type '" + imethod.return_type.cname + "' was not found.");

				output << INDENT3 "ret = "
					   << sformat(return_type->cs_managed_to_variant, "callRet", return_type->cs_type, return_type->name)
					   << ";\n"
					   << INDENT3 "return true;\n";
			} else {
				output << INDENT3 "ret = default;\n"
					   << INDENT3 "return true;\n";
			}

			output << INDENT2 "}\n";
		}

		if (is_derived_type) {
			output << INDENT2 "return base." CS_METHOD_INVOKE_GODOT_CLASS_METHOD "(method, args, out ret);\n";
		} else {
			output << INDENT2 "ret = default;\n"
				   << INDENT2 "return false;\n";
		}

		output << INDENT1 "}\n";

		output << "#pragma warning restore CS0618\n";

		// Generate HasGodotClassMethod

		output << MEMBER_BEGIN "/// <summary>\n"
			   << INDENT1 "/// Check if the type contains a method with the given name.\n"
			   << INDENT1 "/// This method is used by Godot to check if a method exists before invoking it.\n"
			   << INDENT1 "/// Do not call or override this method.\n"
			   << INDENT1 "/// </summary>\n"
			   << INDENT1 "/// <param name=\"method\">Name of the method to check for.</param>\n";

		output << MEMBER_BEGIN "public " << (is_derived_type ? "override" : "virtual")
			   << " bool " CS_METHOD_HAS_GODOT_CLASS_METHOD "(in godot_string_name method)\n"
			   << INDENT1 "{\n";

		for (const MethodInterface &imethod : itype.methods) {
			if (!imethod.is_virtual) {
				continue;
			}

			// We check for native names (snake_case). If we detect one, we call HasGodotClassMethod
			// again, but this time with the respective proxy name (PascalCase). It's the job of
			// user derived classes to override the method and check for those. Our C# source
			// generators take care of generating those override methods.
			output << INDENT2 "if (method == MethodName." << imethod.proxy_name
				   << ")\n" INDENT2 "{\n"
				   << INDENT3 "if (" CS_METHOD_HAS_GODOT_CLASS_METHOD "("
				   << CS_STATIC_FIELD_METHOD_PROXY_NAME_PREFIX << imethod.name
				   << ".NativeValue.DangerousSelfRef))\n" INDENT3 "{\n"
				   << INDENT4 "return true;\n"
				   << INDENT3 "}\n" INDENT2 "}\n";
		}

		if (is_derived_type) {
			output << INDENT2 "return base." CS_METHOD_HAS_GODOT_CLASS_METHOD "(method);\n";
		} else {
			output << INDENT2 "return false;\n";
		}

		output << INDENT1 "}\n";

		// Generate HasGodotClassSignal

		output << MEMBER_BEGIN "/// <summary>\n"
			   << INDENT1 "/// Check if the type contains a signal with the given name.\n"
			   << INDENT1 "/// This method is used by Godot to check if a signal exists before raising it.\n"
			   << INDENT1 "/// Do not call or override this method.\n"
			   << INDENT1 "/// </summary>\n"
			   << INDENT1 "/// <param name=\"signal\">Name of the signal to check for.</param>\n";

		output << MEMBER_BEGIN "public " << (is_derived_type ? "override" : "virtual")
			   << " bool " CS_METHOD_HAS_GODOT_CLASS_SIGNAL "(in godot_string_name signal)\n"
			   << INDENT1 "{\n";

		for (const SignalInterface &isignal : itype.signals_) {
			// We check for native names (snake_case). If we detect one, we call HasGodotClassSignal
			// again, but this time with the respective proxy name (PascalCase). It's the job of
			// user derived classes to override the method and check for those. Our C# source
			// generators take care of generating those override methods.
			output << INDENT2 "if (signal == SignalName." << isignal.proxy_name
				   << ")\n" INDENT2 "{\n"
				   << INDENT3 "if (" CS_METHOD_HAS_GODOT_CLASS_SIGNAL "("
				   << CS_STATIC_FIELD_SIGNAL_PROXY_NAME_PREFIX << isignal.name
				   << ".NativeValue.DangerousSelfRef))\n" INDENT3 "{\n"
				   << INDENT4 "return true;\n"
				   << INDENT3 "}\n" INDENT2 "}\n";
		}

		if (is_derived_type) {
			output << INDENT2 "return base." CS_METHOD_HAS_GODOT_CLASS_SIGNAL "(signal);\n";
		} else {
			output << INDENT2 "return false;\n";
		}

		output << INDENT1 "}\n";
	}

	//Generate StringName for all class members
	bool is_inherit = !itype.is_singleton && obj_types.has(itype.base_name);
	//PropertyName
	output << MEMBER_BEGIN "/// <summary>\n"
		   << INDENT1 "/// Cached StringNames for the properties and fields contained in this class, for fast lookup.\n"
		   << INDENT1 "/// </summary>\n";
	if (is_inherit) {
		output << INDENT1 "public new class PropertyName : " << obj_types[itype.base_name].proxy_name << ".PropertyName";
	} else {
		output << INDENT1 "public class PropertyName";
	}
	output << "\n"
		   << INDENT1 "{\n";
	for (const PropertyInterface &iprop : itype.properties) {
		output << INDENT2 "/// <summary>\n"
			   << INDENT2 "/// Cached name for the '" << iprop.cname << "' property.\n"
			   << INDENT2 "/// </summary>\n"
			   << INDENT2 "public static "
			   << (prop_allowed_inherited_member_hiding.has(itype.proxy_name + ".PropertyName." + iprop.proxy_name) ? "new " : "")
			   << "readonly StringName " << iprop.proxy_name << " = \"" << iprop.cname << "\";\n";
	}
	output << INDENT1 "}\n";
	//MethodName
	output << MEMBER_BEGIN "/// <summary>\n"
		   << INDENT1 "/// Cached StringNames for the methods contained in this class, for fast lookup.\n"
		   << INDENT1 "/// </summary>\n";
	if (is_inherit) {
		output << INDENT1 "public new class MethodName : " << obj_types[itype.base_name].proxy_name << ".MethodName";
	} else {
		output << INDENT1 "public class MethodName";
	}
	output << "\n"
		   << INDENT1 "{\n";
	HashMap<String, StringName> method_names;
	for (const MethodInterface &imethod : itype.methods) {
		if (method_names.has(imethod.proxy_name)) {
			ERR_FAIL_COND_V_MSG(method_names[imethod.proxy_name] != imethod.cname, ERR_BUG, "Method name '" + imethod.proxy_name + "' already exists with a different value.");
			continue;
		}
		method_names[imethod.proxy_name] = imethod.cname;
		output << INDENT2 "/// <summary>\n"
			   << INDENT2 "/// Cached name for the '" << imethod.cname << "' method.\n"
			   << INDENT2 "/// </summary>\n"
			   << INDENT2 "public static "
			   << (prop_allowed_inherited_member_hiding.has(itype.proxy_name + ".MethodName." + imethod.proxy_name) ? "new " : "")
			   << "readonly StringName " << imethod.proxy_name << " = \"" << imethod.cname << "\";\n";
	}
	output << INDENT1 "}\n";
	//SignalName
	output << MEMBER_BEGIN "/// <summary>\n"
		   << INDENT1 "/// Cached StringNames for the signals contained in this class, for fast lookup.\n"
		   << INDENT1 "/// </summary>\n";
	if (is_inherit) {
		output << INDENT1 "public new class SignalName : " << obj_types[itype.base_name].proxy_name << ".SignalName";
	} else {
		output << INDENT1 "public class SignalName";
	}
	output << "\n"
		   << INDENT1 "{\n";
	for (const SignalInterface &isignal : itype.signals_) {
		output << INDENT2 "/// <summary>\n"
			   << INDENT2 "/// Cached name for the '" << isignal.cname << "' signal.\n"
			   << INDENT2 "/// </summary>\n"
			   << INDENT2 "public static "
			   << (prop_allowed_inherited_member_hiding.has(itype.proxy_name + ".SignalName." + isignal.proxy_name) ? "new " : "")
			   << "readonly StringName " << isignal.proxy_name << " = \"" << isignal.cname << "\";\n";
	}
	output << INDENT1 "}\n";

	output.append(CLOSE_BLOCK /* class */);

	return _save_file(p_output_file, output);
}

void RuntimeBindingsGenerator::_append_xml_member(StringBuilder &p_xml_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target,
		const Vector<String> &p_link_target_parts, const TypeInterface *p_source_itype,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	if (p_link_target.contains_char('/')) {
		// Properties with '/' (slash) in the name are not declared in C#, so there is nothing to reference.
		_append_xml_undeclared(p_xml_output, p_link_target);
	} else if (!p_target_itype || !p_target_itype->is_object_type) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve member reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from member reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_xml_undeclared(p_xml_output, p_link_target);
	} else {
		const TypeInterface *current_itype = p_target_itype;
		const PropertyInterface *target_iprop = nullptr;

		while (target_iprop == nullptr && current_itype != nullptr) {
			target_iprop = current_itype->find_property_by_name(p_target_cname);
			if (target_iprop == nullptr) {
				current_itype = _get_type_or_null(TypeReference(current_itype->base_name), builtin_types,
						obj_types, enum_types);
			}
		}

		if (target_iprop) {
			const String member_name = current_itype->proxy_name + "." + target_iprop->proxy_name;
			if (!_validate_api_type(p_target_itype, p_source_itype)) {
				_append_xml_undeclared(p_xml_output, member_name);
			} else {
				p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
				p_xml_output.append(member_name);
				p_xml_output.append("\"/>");
			}
		} else {
			if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
				ERR_PRINT("Cannot resolve member reference in documentation: '" + p_link_target + "'.");
			}

			_append_xml_undeclared(p_xml_output, p_link_target);
		}
	}
}

void RuntimeBindingsGenerator::_append_xml_signal(StringBuilder &p_xml_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target,
		const Vector<String> &p_link_target_parts, const TypeInterface *p_source_itype) {
	if (!p_target_itype || !p_target_itype->is_object_type) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve signal reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from signal reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_xml_undeclared(p_xml_output, p_link_target);
	} else {
		const SignalInterface *target_isignal = p_target_itype->find_signal_by_name(p_target_cname);

		if (target_isignal) {
			const String signal_name = p_target_itype->proxy_name + "." + target_isignal->proxy_name;
			if (!_validate_api_type(p_target_itype, p_source_itype)) {
				_append_xml_undeclared(p_xml_output, signal_name);
			} else {
				p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
				p_xml_output.append(signal_name);
				p_xml_output.append("\"/>");
			}
		} else {
			if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
				ERR_PRINT("Cannot resolve signal reference in documentation: '" + p_link_target + "'.");
			}

			_append_xml_undeclared(p_xml_output, p_link_target);
		}
	}
}

void RuntimeBindingsGenerator::_append_xml_enum(StringBuilder &p_xml_output, const TypeInterface *p_target_itype,
		const StringName &p_target_cname, const String &p_link_target,
		const Vector<String> &p_link_target_parts, const TypeInterface *p_source_itype,
		const HashMap<StringName, TypeInterface> &enum_types) {
	const StringName search_cname = !p_target_itype ? p_target_cname : StringName(p_target_itype->name + "." + (String)p_target_cname);

	HashMap<StringName, TypeInterface>::ConstIterator enum_match = enum_types.find(search_cname);

	if (!enum_match && search_cname != p_target_cname) {
		enum_match = enum_types.find(p_target_cname);
	}

	if (enum_match) {
		const TypeInterface &target_enum_itype = enum_match->value;

		if (!_validate_api_type(p_target_itype, p_source_itype)) {
			_append_xml_undeclared(p_xml_output, target_enum_itype.proxy_name);
		} else {
			p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
			p_xml_output.append(target_enum_itype.proxy_name); // Includes nesting class if any
			p_xml_output.append("\"/>");
		}
	} else {
		if (p_target_itype == nullptr || !p_target_itype->is_intentionally_ignored(p_target_cname)) {
			ERR_PRINT("Cannot resolve enum reference in documentation: '" + p_link_target + "'.");
		}

		_append_xml_undeclared(p_xml_output, p_link_target);
	}
}

void RuntimeBindingsGenerator::_append_xml_constant_in_global_scope(StringBuilder &p_xml_output,
		const String &p_target_cname, const String &p_link_target,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums) {
	// Try to find as a global constant
	const ConstantInterface *target_iconst = find_constant_by_name(p_target_cname, global_constants);

	if (target_iconst) {
		// Found global constant
		p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE "." BINDINGS_GLOBAL_SCOPE_CLASS ".");
		p_xml_output.append(target_iconst->proxy_name);
		p_xml_output.append("\"/>");
	} else {
		// Try to find as global enum constant
		const EnumInterface *target_ienum = nullptr;

		for (const EnumInterface &ienum : global_enums) {
			target_ienum = &ienum;
			target_iconst = find_constant_by_name(p_target_cname, target_ienum->constants);
			if (target_iconst) {
				break;
			}
		}

		if (target_iconst) {
			p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
			p_xml_output.append(target_ienum->proxy_name);
			p_xml_output.append(".");
			p_xml_output.append(target_iconst->proxy_name);
			p_xml_output.append("\"/>");
		} else {
			ERR_PRINT("Cannot resolve global constant reference in documentation: '" + p_link_target + "'.");
			_append_xml_undeclared(p_xml_output, p_link_target);
		}
	}
}

void RuntimeBindingsGenerator::_append_xml_param(StringBuilder &p_xml_output, const String &p_link_target,
		bool p_is_signal) {
	const String link_target = snake_to_camel_case(p_link_target);

	if (!p_is_signal) {
		p_xml_output.append("<paramref name=\"");
		p_xml_output.append(link_target);
		p_xml_output.append("\"/>");
	} else {
		// Documentation in C# is added to an event, not the delegate itself;
		// as such, we treat these parameters as codeblocks instead.
		// See: https://github.com/godotengine/godot/pull/65529
		_append_xml_undeclared(p_xml_output, link_target);
	}
}

void RuntimeBindingsGenerator::_append_xml_constant(StringBuilder &p_xml_output,
		const TypeInterface *p_target_itype, const StringName &p_target_cname,
		const String &p_link_target, const Vector<String> &p_link_target_parts,
		const HashMap<StringName, TypeInterface> &obj_types,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums) {
	if (p_link_target_parts[0] == name_cache.type_at_GlobalScope) {
		_append_xml_constant_in_global_scope(p_xml_output, p_target_cname, p_link_target,
				global_constants, global_enums);
	} else if (!p_target_itype || !p_target_itype->is_object_type) {
		// Search in @GlobalScope as a last resort if no class was specified
		if (p_link_target_parts.size() == 1) {
			_append_xml_constant_in_global_scope(p_xml_output, p_target_cname, p_link_target,
					global_constants, global_enums);
			return;
		}

		if (OS::get_singleton()->is_stdout_verbose()) {
			if (p_target_itype) {
				OS::get_singleton()->print("Cannot resolve constant reference for non-GodotObject type in documentation: %s\n", p_link_target.utf8().get_data());
			} else {
				OS::get_singleton()->print("Cannot resolve type from constant reference in documentation: %s\n", p_link_target.utf8().get_data());
			}
		}

		// TODO Map what we can
		_append_xml_undeclared(p_xml_output, p_link_target);
	} else {
		// Try to find the constant in the current class
		if (p_target_itype->is_singleton_instance) {
			// Constants and enums are declared in the static singleton class.
			p_target_itype = &obj_types[p_target_itype->cname];
		}

		const ConstantInterface *target_iconst = find_constant_by_name(p_target_cname, p_target_itype->constants);

		if (target_iconst) {
			// Found constant in current class
			p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
			p_xml_output.append(p_target_itype->proxy_name);
			p_xml_output.append(".");
			p_xml_output.append(target_iconst->proxy_name);
			p_xml_output.append("\"/>");
		} else {
			// Try to find as enum constant in the current class
			const EnumInterface *target_ienum = nullptr;

			for (const EnumInterface &ienum : p_target_itype->enums) {
				target_ienum = &ienum;
				target_iconst = find_constant_by_name(p_target_cname, target_ienum->constants);
				if (target_iconst) {
					break;
				}
			}

			if (target_iconst) {
				p_xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
				p_xml_output.append(p_target_itype->proxy_name);
				p_xml_output.append(".");
				p_xml_output.append(target_ienum->proxy_name);
				p_xml_output.append(".");
				p_xml_output.append(target_iconst->proxy_name);
				p_xml_output.append("\"/>");
			} else if (p_link_target_parts.size() == 1) {
				// Also search in @GlobalScope as a last resort if no class was specified
				_append_xml_constant_in_global_scope(p_xml_output, p_target_cname, p_link_target,
						global_constants, global_enums);
			} else {
				if (!p_target_itype->is_intentionally_ignored(p_target_cname)) {
					ERR_PRINT("Cannot resolve constant reference in documentation: '" + p_link_target + "'.");
				}

				_append_xml_undeclared(p_xml_output, p_link_target);
			}
		}
	}
}

String RuntimeBindingsGenerator::bbcode_to_xml(const String &p_bbcode, const TypeInterface *p_itype,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types,
		const List<ConstantInterface> &global_constants,
		const List<EnumInterface> &global_enums,
		bool p_is_signal) {
	// Based on the version in EditorHelp.

	if (p_bbcode.is_empty()) {
		return String();
	}

	DocTools *doc = EditorHelp::get_doc_data();

	String bbcode = p_bbcode;

	StringBuilder xml_output;

	xml_output.append("<para>");

	List<String> tag_stack;
	bool code_tag = false;
	bool line_del = false;

	int pos = 0;
	while (pos < bbcode.length()) {
		int brk_pos = bbcode.find_char('[', pos);

		if (brk_pos < 0) {
			brk_pos = bbcode.length();
		}

		if (brk_pos > pos) {
			if (!line_del) {
				String text = bbcode.substr(pos, brk_pos - pos);
				if (code_tag || tag_stack.size() > 0) {
					xml_output.append(text.xml_escape());
				} else {
					Vector<String> lines = text.split("\n");
					for (int i = 0; i < lines.size(); i++) {
						if (i != 0) {
							xml_output.append("<para>");
						}

						xml_output.append(lines[i].xml_escape());

						if (i != lines.size() - 1) {
							xml_output.append("</para>\n");
						}
					}
				}
			}
		}

		if (brk_pos == bbcode.length()) {
			// Nothing else to add.
			break;
		}

		int brk_end = bbcode.find_char(']', brk_pos + 1);

		if (brk_end == -1) {
			if (!line_del) {
				String text = bbcode.substr(brk_pos);
				if (code_tag || tag_stack.size() > 0) {
					xml_output.append(text.xml_escape());
				} else {
					Vector<String> lines = text.split("\n");
					for (int i = 0; i < lines.size(); i++) {
						if (i != 0) {
							xml_output.append("<para>");
						}

						xml_output.append(lines[i].xml_escape());

						if (i != lines.size() - 1) {
							xml_output.append("</para>\n");
						}
					}
				}
			}

			break;
		}

		String tag = bbcode.substr(brk_pos + 1, brk_end - brk_pos - 1);

		if (tag.begins_with("/")) {
			bool tag_ok = tag_stack.size() && tag_stack.front()->get() == tag.substr(1);

			if (!tag_ok) {
				if (!line_del) {
					xml_output.append("[");
				}
				pos = brk_pos + 1;
				continue;
			}

			tag_stack.pop_front();
			pos = brk_end + 1;
			code_tag = false;

			if (tag == "/url") {
				xml_output.append("</a>");
			} else if (tag == "/code") {
				xml_output.append("</c>");
			} else if (tag == "/codeblock") {
				xml_output.append("</code>");
			} else if (tag == "/b") {
				xml_output.append("</b>");
			} else if (tag == "/i") {
				xml_output.append("</i>");
			} else if (tag == "/csharp") {
				xml_output.append("</code>");
				line_del = true;
			} else if (tag == "/codeblocks") {
				line_del = false;
			}
		} else if (code_tag) {
			xml_output.append("[");
			pos = brk_pos + 1;
		} else if (tag.begins_with("method ") || tag.begins_with("constructor ") || tag.begins_with("operator ") || tag.begins_with("member ") || tag.begins_with("signal ") || tag.begins_with("enum ") || tag.begins_with("constant ") || tag.begins_with("theme_item ") || tag.begins_with("param ")) {
			const int tag_end = tag.find_char(' ');
			const String link_tag = tag.substr(0, tag_end);
			const String link_target = tag.substr(tag_end + 1).lstrip(" ");

			const Vector<String> link_target_parts = link_target.split(".");

			if (link_target_parts.is_empty() || link_target_parts.size() > 2) {
				ERR_PRINT("Invalid reference format: '" + tag + "'.");

				xml_output.append("<c>");
				xml_output.append(tag);
				xml_output.append("</c>");

				pos = brk_end + 1;
				continue;
			}

			const TypeInterface *target_itype;
			StringName target_cname;

			if (link_target_parts.size() == 2) {
				target_itype = _get_type_or_null(TypeReference(link_target_parts[0]), builtin_types,
						obj_types, enum_types);
				if (!target_itype) {
					target_itype = _get_type_or_null(TypeReference("_" + link_target_parts[0]),
							builtin_types, obj_types, enum_types);
				}
				target_cname = link_target_parts[1];
			} else {
				target_itype = p_itype;
				target_cname = link_target_parts[0];
			}

			if (!_validate_api_type(target_itype, p_itype)) {
				// If the target member is referenced from a type with a different API level, we can't reference it.
				_append_xml_undeclared(xml_output, link_target);
			} else if (link_tag == "method") {
				_append_xml_method(xml_output, target_itype, target_cname, link_target, link_target_parts, p_itype,
						builtin_types, obj_types, enum_types);
			} else if (link_tag == "constructor") {
				// TODO: Support constructors?
				_append_xml_undeclared(xml_output, link_target);
			} else if (link_tag == "operator") {
				// TODO: Support operators?
				_append_xml_undeclared(xml_output, link_target);
			} else if (link_tag == "member") {
				_append_xml_member(xml_output, target_itype, target_cname, link_target, link_target_parts, p_itype,
						builtin_types, obj_types, enum_types);
			} else if (link_tag == "signal") {
				_append_xml_signal(xml_output, target_itype, target_cname, link_target, link_target_parts, p_itype);
			} else if (link_tag == "enum") {
				_append_xml_enum(xml_output, target_itype, target_cname, link_target, link_target_parts, p_itype,
						enum_types);
			} else if (link_tag == "constant") {
				_append_xml_constant(xml_output, target_itype, target_cname, link_target, link_target_parts,
						obj_types, global_constants, global_enums);
			} else if (link_tag == "param") {
				_append_xml_param(xml_output, link_target, p_is_signal);
			} else if (link_tag == "theme_item") {
				// We do not declare theme_items in any way in C#, so there is nothing to reference.
				_append_xml_undeclared(xml_output, link_target);
			}

			pos = brk_end + 1;
		} else if (doc->class_list.has(tag)) {
			if (tag == "Array" || tag == "Dictionary") {
				xml_output.append("<see cref=\"" BINDINGS_NAMESPACE_COLLECTIONS ".");
				xml_output.append(tag);
				xml_output.append("\"/>");
			} else if (tag == "bool" || tag == "int") {
				xml_output.append("<see cref=\"");
				xml_output.append(tag);
				xml_output.append("\"/>");
			} else if (tag == "float") {
				xml_output.append("<see cref=\""
#ifdef REAL_T_IS_DOUBLE
								  "double"
#else
								  "float"
#endif
								  "\"/>");
			} else if (tag == "Variant") {
				xml_output.append("<see cref=\"Godot.Variant\"/>");
			} else if (tag == "String") {
				xml_output.append("<see cref=\"string\"/>");
			} else if (tag == "Nil") {
				xml_output.append("<see langword=\"null\"/>");
			} else if (tag.begins_with("@")) {
				// @GlobalScope, @GDScript, etc.
				xml_output.append("<c>");
				xml_output.append(tag);
				xml_output.append("</c>");
			} else if (tag == "PackedByteArray") {
				xml_output.append("<see cref=\"byte\"/>[]");
			} else if (tag == "PackedInt32Array") {
				xml_output.append("<see cref=\"int\"/>[]");
			} else if (tag == "PackedInt64Array") {
				xml_output.append("<see cref=\"long\"/>[]");
			} else if (tag == "PackedFloat32Array") {
				xml_output.append("<see cref=\"float\"/>[]");
			} else if (tag == "PackedFloat64Array") {
				xml_output.append("<see cref=\"double\"/>[]");
			} else if (tag == "PackedStringArray") {
				xml_output.append("<see cref=\"string\"/>[]");
			} else if (tag == "PackedVector2Array") {
				xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".Vector2\"/>[]");
			} else if (tag == "PackedVector3Array") {
				xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".Vector3\"/>[]");
			} else if (tag == "PackedColorArray") {
				xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".Color\"/>[]");
			} else if (tag == "PackedVector4Array") {
				xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".Vector4\"/>[]");
			} else {
				const TypeInterface *target_itype = _get_type_or_null(TypeReference(tag),
						builtin_types, obj_types, enum_types);

				if (!target_itype) {
					target_itype = _get_type_or_null(TypeReference("_" + tag),
							builtin_types, obj_types, enum_types);
				}

				if (target_itype) {
					if (!_validate_api_type(target_itype, p_itype)) {
						_append_xml_undeclared(xml_output, target_itype->proxy_name);
					} else {
						xml_output.append("<see cref=\"" BINDINGS_NAMESPACE ".");
						xml_output.append(target_itype->proxy_name);
						xml_output.append("\"/>");
					}
				} else {
					ERR_PRINT("Cannot resolve type reference in documentation: '" + tag + "'.");

					xml_output.append("<c>");
					xml_output.append(tag);
					xml_output.append("</c>");
				}
			}

			pos = brk_end + 1;
		} else if (tag == "b") {
			xml_output.append("<b>");

			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "i") {
			xml_output.append("<i>");

			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "code" || tag.begins_with("code ")) {
			int end = bbcode.find_char('[', brk_end);
			if (end == -1) {
				end = bbcode.length();
			}
			String code = bbcode.substr(brk_end + 1, end - brk_end - 1);
			if (langword_check.has(code)) {
				xml_output.append("<see langword=\"");
				xml_output.append(code);
				xml_output.append("\"/>");

				pos = brk_end + code.length() + 8;
			} else {
				xml_output.append("<c>");

				code_tag = true;
				pos = brk_end + 1;
				tag_stack.push_front("code");
			}
		} else if (tag == "codeblock" || tag.begins_with("codeblock ")) {
			xml_output.append("<code>");

			code_tag = true;
			pos = brk_end + 1;
			tag_stack.push_front("codeblock");
		} else if (tag == "codeblocks") {
			line_del = true;
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "csharp" || tag.begins_with("csharp ")) {
			xml_output.append("<code>");

			line_del = false;
			code_tag = true;
			pos = brk_end + 1;
			tag_stack.push_front("csharp");
		} else if (tag == "kbd") {
			// Keyboard combinations are not supported in xml comments.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "center") {
			// Center alignment is not supported in xml comments.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "br") {
			xml_output.append("\n"); // FIXME: Should use <para> instead. Luckily this tag isn't used for now.
			pos = brk_end + 1;
		} else if (tag == "u") {
			// Underline is not supported in Rider xml comments.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "s") {
			// Strikethrough is not supported in xml comments.
			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag == "url") {
			int end = bbcode.find_char('[', brk_end);
			if (end == -1) {
				end = bbcode.length();
			}
			String url = bbcode.substr(brk_end + 1, end - brk_end - 1);
			xml_output.append("<a href=\"");
			xml_output.append(url);
			xml_output.append("\">");
			xml_output.append(url);

			pos = brk_end + 1;
			tag_stack.push_front(tag);
		} else if (tag.begins_with("url=")) {
			String url = tag.substr(4);
			xml_output.append("<a href=\"");
			xml_output.append(url);
			xml_output.append("\">");

			pos = brk_end + 1;
			tag_stack.push_front("url");
		} else if (tag == "img") {
			int end = bbcode.find_char('[', brk_end);
			if (end == -1) {
				end = bbcode.length();
			}
			String image = bbcode.substr(brk_end + 1, end - brk_end - 1);

			// Not supported. Just append the bbcode.
			xml_output.append("[img]");
			xml_output.append(image);
			xml_output.append("[/img]");

			pos = end;
			tag_stack.push_front(tag);
		} else if (tag.begins_with("color=")) {
			// Not supported.
			pos = brk_end + 1;
			tag_stack.push_front("color");
		} else if (tag.begins_with("font=")) {
			// Not supported.
			pos = brk_end + 1;
			tag_stack.push_front("font");
		} else {
			if (!line_del) {
				// Ignore unrecognized tag.
				xml_output.append("[");
			}
			pos = brk_pos + 1;
		}
	}

	xml_output.append("</para>");

	return xml_output.as_string();
}

const TypeInterface *RuntimeBindingsGenerator::_get_type_or_null(const TypeReference &p_typeref,
		const HashMap<StringName, TypeInterface> &builtin_types,
		const HashMap<StringName, TypeInterface> &obj_types,
		const HashMap<StringName, TypeInterface> &enum_types) {
	HashMap<StringName, TypeInterface>::ConstIterator builtin_type_match = builtin_types.find(p_typeref.cname);

	if (builtin_type_match) {
		return &builtin_type_match->value;
	}

	HashMap<StringName, TypeInterface>::ConstIterator obj_type_match = obj_types.find(p_typeref.cname);

	if (obj_type_match) {
		return &obj_type_match->value;
	}

	if (p_typeref.is_enum) {
		HashMap<StringName, TypeInterface>::ConstIterator enum_match = enum_types.find(p_typeref.cname);

		if (enum_match) {
			return &enum_match->value;
		}

		// Enum not found. Most likely because none of its constants were bound, so it's empty. That's fine. Use int instead.
		HashMap<StringName, TypeInterface>::ConstIterator int_match = builtin_types.find(name_cache.type_int);
		ERR_FAIL_NULL_V(int_match, nullptr);
		return &int_match->value;
	}

	return nullptr;
}

bool RuntimeBindingsGenerator::_validate_api_type(const TypeInterface *p_target_itype,
		const TypeInterface *p_source_itype) {
	static constexpr const char *api_types[5] = {
		"Core",
		"Editor",
		"Extension",
		"Editor Extension",
		"None",
	};

	const ClassDB::APIType target_api = p_target_itype ? p_target_itype->api_type : ClassDB::API_NONE;
	ERR_FAIL_INDEX_V((int)target_api, 5, false);
	const ClassDB::APIType source_api = p_source_itype ? p_source_itype->api_type : ClassDB::API_NONE;
	ERR_FAIL_INDEX_V((int)source_api, 5, false);
	bool validate = false;

	switch (target_api) {
		case ClassDB::API_NONE:
		case ClassDB::API_CORE:
		default:
			validate = true;
			break;
		case ClassDB::API_EDITOR:
			validate = source_api == ClassDB::API_EDITOR || source_api == ClassDB::API_EDITOR_EXTENSION;
			break;
		case ClassDB::API_EXTENSION:
			validate = source_api == ClassDB::API_EXTENSION || source_api == ClassDB::API_EDITOR_EXTENSION;
			break;
		case ClassDB::API_EDITOR_EXTENSION:
			validate = source_api == ClassDB::API_EDITOR_EXTENSION;
			break;
	}
	if (!validate) {
		const String target_name = p_target_itype ? p_target_itype->proxy_name : "@GlobalScope";
		const String source_name = p_source_itype ? p_source_itype->proxy_name : "@GlobalScope";
		WARN_PRINT(vformat("Type '%s' has API level '%s'; it cannot be referenced by type '%s' with API level '%s'.",
				target_name, api_types[target_api], source_name, api_types[source_api]));
	}
	return validate;
}
