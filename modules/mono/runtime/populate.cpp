#include "runtime_bindings_generator.h"
#include "../utils/naming_utils.h"
#include "../utils/string_utils.h"
#include "../bindings_generator_defs.h"

using TypeInterface = RuntimeBindingsGenerator::TypeInterface;

struct SortMethodWithHashes {
	_FORCE_INLINE_ bool operator()(const Pair<MethodInfo, uint32_t> &p_a, const Pair<MethodInfo, uint32_t> &p_b) const {
		return p_a.first < p_b.first;
	}
};

// Types that will be ignored by the generator and won't be available in C#.
// This must be kept in sync with `ignored_types` in csharp_script.cpp
const Vector<String> ignored_types = {};
// Special [code] keywords to wrap with <see langword="code"/> instead of <c>code</c>.
// Don't check against all C# reserved words, as many cases are GDScript-specific.
const Vector<String> langword_check = { "true", "false", "null" };

bool method_has_ptr_parameter(MethodInfo p_method_info) {
	if (p_method_info.return_val.type == Variant::INT &&
			p_method_info.return_val.hint == PROPERTY_HINT_INT_IS_POINTER) {
		return true;
	}
	for (PropertyInfo arg : p_method_info.arguments) {
		if (arg.type == Variant::INT && arg.hint == PROPERTY_HINT_INT_IS_POINTER) {
			return true;
		}
	}
	return false;
}

static String _get_vector2_cs_ctor_args(const Vector2 &p_vec2) {
	return String::num_real(p_vec2.x, true) + "f, " +
			String::num_real(p_vec2.y, true) + "f";
}

static String _get_vector3_cs_ctor_args(const Vector3 &p_vec3) {
	return String::num_real(p_vec3.x, true) + "f, " +
			String::num_real(p_vec3.y, true) + "f, " +
			String::num_real(p_vec3.z, true) + "f";
}

static String _get_vector4_cs_ctor_args(const Vector4 &p_vec4) {
	return String::num_real(p_vec4.x, true) + "f, " +
			String::num_real(p_vec4.y, true) + "f, " +
			String::num_real(p_vec4.z, true) + "f, " +
			String::num_real(p_vec4.w, true) + "f";
}

static String _get_vector2i_cs_ctor_args(const Vector2i &p_vec2i) {
	return itos(p_vec2i.x) + ", " + itos(p_vec2i.y);
}

static String _get_vector3i_cs_ctor_args(const Vector3i &p_vec3i) {
	return itos(p_vec3i.x) + ", " + itos(p_vec3i.y) + ", " + itos(p_vec3i.z);
}

static String _get_vector4i_cs_ctor_args(const Vector4i &p_vec4i) {
	return itos(p_vec4i.x) + ", " + itos(p_vec4i.y) + ", " + itos(p_vec4i.z) + ", " + itos(p_vec4i.w);
}

static String _get_color_cs_ctor_args(const Color &p_color) {
	return String::num(p_color.r, 4) + "f, " +
			String::num(p_color.g, 4) + "f, " +
			String::num(p_color.b, 4) + "f, " +
			String::num(p_color.a, 4) + "f";
}

bool RuntimeBindingsGenerator::_populate_object_type_interfaces(const LocalVector<StringName> &class_list,
		HashMap<StringName, TypeInterface> &out_obj_types,
		HashMap<StringName, TypeInterface> &out_enum_types) {
	for (const StringName &type_cname : class_list) {
		ClassDB::APIType api_type = ClassDB::get_api_type(type_cname);

		if (api_type == ClassDB::API_NONE) {
			continue;
		}

		if (ignored_types.has(type_cname)) {
			_log("Ignoring type '%s' because it's in the list of ignored types\n", String(type_cname).utf8().get_data());
			continue;
		}

		if (!ClassDB::is_class_exposed(type_cname)) {
			_log("Ignoring type '%s' because it's not exposed\n", String(type_cname).utf8().get_data());
			continue;
		}

		if (!ClassDB::is_class_enabled(type_cname)) {
			_log("Ignoring type '%s' because it's not enabled\n", String(type_cname).utf8().get_data());
			continue;
		}

		TypeInterface itype = TypeInterface::create_object_type(type_cname, pascal_to_pascal_case(type_cname), api_type);
		_initialize_type_interface(type_cname, itype);

		// Populate properties

		List<PropertyInfo> property_list;
		ClassDB::get_property_list(type_cname, &property_list, true);

		HashMap<StringName, StringName> accessor_methods;

		for (const PropertyInfo &property : property_list) {
			if (property.usage & PROPERTY_USAGE_GROUP || property.usage & PROPERTY_USAGE_SUBGROUP || property.usage & PROPERTY_USAGE_CATEGORY || (property.type == Variant::NIL && property.usage & PROPERTY_USAGE_ARRAY)) {
				continue;
			}

			if (property.name.contains_char('/')) {
				// Ignore properties with '/' (slash) in the name. These are only meant for use in the inspector.
				continue;
			}

			PropertyInterface iprop;
			auto err = _populate_property_interface(property, type_cname, itype, accessor_methods, iprop);
			if (err != OK) {
				return false;
			}
			itype.properties.push_back(iprop);
		}

		// Populate methods

		List<MethodInfo> virtual_method_list;
		ClassDB::get_virtual_methods(type_cname, &virtual_method_list, true);

		List<Pair<MethodInfo, uint32_t>> method_list_with_hashes;
		ClassDB::get_method_list_with_compatibility(type_cname, &method_list_with_hashes, true);
		method_list_with_hashes.sort_custom<SortMethodWithHashes>();

		List<MethodInterface> compat_methods;
		for (const Pair<MethodInfo, uint32_t> &E : method_list_with_hashes) {
			const MethodInfo &method_info = E.first;
			const uint32_t hash = E.second;

			if (method_info.name.is_empty()) {
				continue;
			}

			String cname = method_info.name;

			if (blacklisted_methods.find(itype.cname) && blacklisted_methods[itype.cname].find(cname)) {
				continue;
			}

			if (method_has_ptr_parameter(method_info)) {
				// Pointers are not supported.
				itype.ignored_members.insert(method_info.name);
				continue;
			}

			MethodInterface imethod;
			imethod.name = method_info.name;
			imethod.cname = cname;
			imethod.hash = hash;

			if (method_info.flags & METHOD_FLAG_STATIC) {
				imethod.is_static = true;
			}

			if (method_info.flags & METHOD_FLAG_VIRTUAL) {
				imethod.is_virtual = true;
				itype.has_virtual_methods = true;
			}

			PropertyInfo return_info = method_info.return_val;

			MethodBind *m = nullptr;

			if (!imethod.is_virtual) {
				bool method_exists = false;
				m = ClassDB::get_method_with_compatibility(type_cname, method_info.name, hash, &method_exists, &imethod.is_compat);

				if (unlikely(!method_exists)) {
					ERR_FAIL_COND_V_MSG(!virtual_method_list.find(method_info), false,
							"Missing MethodBind for non-virtual method: '" + itype.name + "." + imethod.name + "'.");
				}
			}

			imethod.is_vararg = m && m->is_vararg();

			if (!m && !imethod.is_virtual) {
				ERR_FAIL_COND_V_MSG(!virtual_method_list.find(method_info), false,
						"Missing MethodBind for non-virtual method: '" + itype.name + "." + imethod.name + "'.");

				// A virtual method without the virtual flag. This is a special case.

				// There is no method bind, so let's fallback to Godot's object.Call(string, params)
				imethod.requires_object_call = true;

				// The method Object.free is registered as a virtual method, but without the virtual flag.
				// This is because this method is not supposed to be overridden, but called.
				// We assume the return type is void.
				imethod.return_type.cname = name_cache.type_void;

				// Actually, more methods like this may be added in the future, which could return
				// something different. Let's put this check to notify us if that ever happens.
				if (itype.cname != name_cache.type_Object || imethod.name != "free") {
					WARN_PRINT("Notification: New unexpected virtual non-overridable method found."
							   " We only expected Object.free, but found '" +
							itype.name + "." + imethod.name + "'.");
				}
			} else if (return_info.type == Variant::INT && return_info.usage & (PROPERTY_USAGE_CLASS_IS_ENUM | PROPERTY_USAGE_CLASS_IS_BITFIELD)) {
				imethod.return_type.cname = return_info.class_name;
				imethod.return_type.is_enum = true;
			} else if (return_info.class_name != StringName()) {
				imethod.return_type.cname = return_info.class_name;

				bool bad_reference_hint = !imethod.is_virtual && return_info.hint != PROPERTY_HINT_RESOURCE_TYPE &&
						ClassDB::is_parent_class(return_info.class_name, name_cache.type_RefCounted);
				ERR_FAIL_COND_V_MSG(bad_reference_hint, false,
						String() + "Return type is reference but hint is not '" _STR(PROPERTY_HINT_RESOURCE_TYPE) "'." +
								" Are you returning a reference type by pointer? Method: '" + itype.name + "." + imethod.name + "'.");
			} else if (return_info.type == Variant::ARRAY && return_info.hint == PROPERTY_HINT_ARRAY_TYPE) {
				imethod.return_type.cname = Variant::get_type_name(return_info.type) + "_@generic";
				imethod.return_type.generic_type_parameters.push_back(TypeReference(return_info.hint_string));
			} else if (return_info.type == Variant::DICTIONARY && return_info.hint == PROPERTY_HINT_DICTIONARY_TYPE) {
				imethod.return_type.cname = Variant::get_type_name(return_info.type) + "_@generic";
				Vector<String> split = return_info.hint_string.split(";");
				imethod.return_type.generic_type_parameters.push_back(TypeReference(split.get(0)));
				imethod.return_type.generic_type_parameters.push_back(TypeReference(split.get(1)));
			} else if (return_info.hint == PROPERTY_HINT_RESOURCE_TYPE) {
				imethod.return_type.cname = return_info.hint_string;
			} else if (return_info.type == Variant::NIL && return_info.usage & PROPERTY_USAGE_NIL_IS_VARIANT) {
				imethod.return_type.cname = name_cache.type_Variant;
			} else if (return_info.type == Variant::NIL) {
				imethod.return_type.cname = name_cache.type_void;
			} else {
				imethod.return_type.cname = _get_type_name_from_meta(return_info.type, m ? m->get_argument_meta(-1) : (GodotTypeInfo::Metadata)method_info.return_val_metadata);
			}

			for (int64_t idx = 0; idx < method_info.arguments.size(); ++idx) {
				const PropertyInfo &arginfo = method_info.arguments[idx];

				String orig_arg_name = arginfo.name;

				ArgumentInterface iarg;
				iarg.name = orig_arg_name;

				if (arginfo.type == Variant::INT && arginfo.usage & (PROPERTY_USAGE_CLASS_IS_ENUM | PROPERTY_USAGE_CLASS_IS_BITFIELD)) {
					iarg.type.cname = arginfo.class_name;
					iarg.type.is_enum = true;
				} else if (arginfo.class_name != StringName()) {
					iarg.type.cname = arginfo.class_name;
				} else if (arginfo.type == Variant::ARRAY && arginfo.hint == PROPERTY_HINT_ARRAY_TYPE) {
					iarg.type.cname = Variant::get_type_name(arginfo.type) + "_@generic";
					iarg.type.generic_type_parameters.push_back(TypeReference(arginfo.hint_string));
				} else if (arginfo.type == Variant::DICTIONARY && arginfo.hint == PROPERTY_HINT_DICTIONARY_TYPE) {
					iarg.type.cname = Variant::get_type_name(arginfo.type) + "_@generic";
					Vector<String> split = arginfo.hint_string.split(";");
					iarg.type.generic_type_parameters.push_back(TypeReference(split.get(0)));
					iarg.type.generic_type_parameters.push_back(TypeReference(split.get(1)));
				} else if (arginfo.hint == PROPERTY_HINT_RESOURCE_TYPE) {
					iarg.type.cname = arginfo.hint_string;
				} else if (arginfo.type == Variant::NIL) {
					iarg.type.cname = name_cache.type_Variant;
				} else {
					iarg.type.cname = _get_type_name_from_meta(arginfo.type, m ? m->get_argument_meta(idx) : (GodotTypeInfo::Metadata)method_info.get_argument_meta(idx));
				}

				iarg.name = escape_csharp_keyword(snake_to_camel_case(iarg.name));

				if (m && m->has_default_argument(idx)) {
					bool defval_ok = _arg_default_value_from_variant(m->get_default_argument(idx), iarg);
					ERR_FAIL_COND_V_MSG(!defval_ok, false,
							"Cannot determine default value for argument '" + orig_arg_name + "' of method '" + itype.name + "." + imethod.name + "'.");
				}

				imethod.add_argument(iarg);
			}

			if (imethod.is_vararg) {
				ArgumentInterface ivararg;
				ivararg.type.cname = name_cache.type_VarArg;
				ivararg.name = "@args";
				imethod.add_argument(ivararg);
			}

			imethod.proxy_name = escape_csharp_keyword(snake_to_pascal_case(imethod.name));

			// Prevent the method and its enclosing type from sharing the same name
			if (imethod.proxy_name == itype.proxy_name) {
				_log("Name of method '%s' is ambiguous with the name of its enclosing class '%s'. Renaming method to '%s_'\n",
						imethod.proxy_name.utf8().get_data(), itype.proxy_name.utf8().get_data(), imethod.proxy_name.utf8().get_data());

				imethod.proxy_name += "_";
			}

			HashMap<StringName, StringName>::Iterator accessor = accessor_methods.find(imethod.cname);
			if (accessor) {
				// We only hide an accessor method if it's in the same class as the property.
				// It's easier this way, but also we don't know if an accessor method in a different class
				// could have other purposes, so better leave those untouched.
				imethod.is_hidden = true;
			}

			if (itype.class_doc) {
				for (int i = 0; i < itype.class_doc->methods.size(); i++) {
					if (itype.class_doc->methods[i].name == imethod.name) {
						imethod.method_doc = &itype.class_doc->methods[i];
						break;
					}
				}
			}

			if (imethod.method_doc) {
				imethod.is_deprecated = imethod.method_doc->is_deprecated;
				imethod.deprecation_message = imethod.method_doc->deprecated_message;

				if (imethod.is_deprecated && imethod.deprecation_message.is_empty()) {
					WARN_PRINT("An empty deprecation message is discouraged. Method: '" + itype.proxy_name + "." + imethod.proxy_name + "'.");
					imethod.deprecation_message = "This method is deprecated.";
				}
			}

			ERR_FAIL_COND_V_MSG(itype.find_property_by_name(imethod.cname), false,
					"Method name conflicts with property: '" + itype.name + "." + imethod.name + "'.");

			// Compat methods aren't added to the type yet, they need to be checked for conflicts
			// after all the non-compat methods have been added. The compat methods are added in
			// reverse so the most recently added ones take precedence over older compat methods.
			if (imethod.is_compat) {
				// If the method references deprecated types, mark the method as deprecated as well.
				for (const ArgumentInterface &iarg : imethod.arguments) {
					String arg_type_name = iarg.type.cname;
					String doc_name = arg_type_name.begins_with("_") ? arg_type_name.substr(1) : arg_type_name;
					const DocData::ClassDoc &class_doc = EditorHelp::get_doc_data()->class_list[doc_name];
					if (class_doc.is_deprecated) {
						imethod.is_deprecated = true;
						imethod.deprecation_message = "This method overload is deprecated.";
						break;
					}
				}

				imethod.is_hidden = true;
				compat_methods.push_front(imethod);
				continue;
			}

			// Methods starting with an underscore are ignored unless they're used as a property setter or getter
			if (!imethod.is_virtual && imethod.name[0] == '_') {
				for (const PropertyInterface &iprop : itype.properties) {
					if (iprop.setter == imethod.name || iprop.getter == imethod.name) {
						imethod.is_internal = true;
						itype.methods.push_back(imethod);
						break;
					}
				}
			} else {
				itype.methods.push_back(imethod);
			}
		}

		// Add compat methods that don't conflict with other methods in the type.
		for (const MethodInterface &imethod : compat_methods) {
			if (_method_has_conflicting_signature(imethod, itype)) {
				WARN_PRINT("Method '" + imethod.name + "' conflicts with an already existing method in type '" + itype.name + "' and has been ignored.");
				continue;
			}
			itype.methods.push_back(imethod);
		}

		// Populate signals
		ClassDB::ClassInfo *class_info = ClassDB::classes.getptr(type_cname);
		const AHashMap<StringName, MethodInfo> &signal_map = class_info->signal_map;

		for (const KeyValue<StringName, MethodInfo> &E : signal_map) {
			SignalInterface isignal;

			const MethodInfo &method_info = E.value;

			isignal.name = method_info.name;
			isignal.cname = method_info.name;

			for (int64_t idx = 0; idx < method_info.arguments.size(); ++idx) {
				const PropertyInfo &arginfo = method_info.arguments[idx];

				String orig_arg_name = arginfo.name;

				ArgumentInterface iarg;
				iarg.name = orig_arg_name;

				if (arginfo.type == Variant::INT && arginfo.usage & (PROPERTY_USAGE_CLASS_IS_ENUM | PROPERTY_USAGE_CLASS_IS_BITFIELD)) {
					iarg.type.cname = arginfo.class_name;
					iarg.type.is_enum = true;
				} else if (arginfo.class_name != StringName()) {
					iarg.type.cname = arginfo.class_name;
				} else if (arginfo.type == Variant::ARRAY && arginfo.hint == PROPERTY_HINT_ARRAY_TYPE) {
					iarg.type.cname = Variant::get_type_name(arginfo.type) + "_@generic";
					iarg.type.generic_type_parameters.push_back(TypeReference(arginfo.hint_string));
				} else if (arginfo.type == Variant::DICTIONARY && arginfo.hint == PROPERTY_HINT_DICTIONARY_TYPE) {
					iarg.type.cname = Variant::get_type_name(arginfo.type) + "_@generic";
					Vector<String> split = arginfo.hint_string.split(";");
					iarg.type.generic_type_parameters.push_back(TypeReference(split.get(0)));
					iarg.type.generic_type_parameters.push_back(TypeReference(split.get(1)));
				} else if (arginfo.hint == PROPERTY_HINT_RESOURCE_TYPE) {
					iarg.type.cname = arginfo.hint_string;
				} else if (arginfo.type == Variant::NIL) {
					iarg.type.cname = name_cache.type_Variant;
				} else {
					iarg.type.cname = _get_type_name_from_meta(arginfo.type, (GodotTypeInfo::Metadata)method_info.get_argument_meta(idx));
				}

				iarg.name = escape_csharp_keyword(snake_to_camel_case(iarg.name));

				isignal.add_argument(iarg);
			}

			isignal.proxy_name = escape_csharp_keyword(snake_to_pascal_case(isignal.name));

			// Prevent the signal and its enclosing type from sharing the same name
			if (isignal.proxy_name == itype.proxy_name) {
				_log("Name of signal '%s' is ambiguous with the name of its enclosing class '%s'. Renaming signal to '%s_'\n",
						isignal.proxy_name.utf8().get_data(), itype.proxy_name.utf8().get_data(), isignal.proxy_name.utf8().get_data());

				isignal.proxy_name += "_";
			}

			if (itype.find_property_by_proxy_name(isignal.proxy_name) || itype.find_method_by_proxy_name(isignal.proxy_name)) {
				// ClassDB allows signal names that conflict with method or property names.
				// While registering a signal with a conflicting name is considered wrong,
				// it may still happen and it may take some time until someone fixes the name.
				// We can't allow the bindings to be in a broken state while we wait for a fix;
				// that's why we must handle this possibility by renaming the signal.
				isignal.proxy_name += "Signal";
			}

			if (itype.class_doc) {
				for (int i = 0; i < itype.class_doc->signals.size(); i++) {
					const DocData::MethodDoc &signal_doc = itype.class_doc->signals[i];
					if (signal_doc.name == isignal.name) {
						isignal.method_doc = &signal_doc;
						break;
					}
				}
			}

			if (isignal.method_doc) {
				isignal.is_deprecated = isignal.method_doc->is_deprecated;
				isignal.deprecation_message = isignal.method_doc->deprecated_message;

				if (isignal.is_deprecated && isignal.deprecation_message.is_empty()) {
					WARN_PRINT("An empty deprecation message is discouraged. Signal: '" + itype.proxy_name + "." + isignal.proxy_name + "'.");
					isignal.deprecation_message = "This signal is deprecated.";
				}
			}

			itype.signals_.push_back(isignal);
		}

		// Populate enums and constants

		List<String> constants;
		ClassDB::get_integer_constant_list(type_cname, &constants, true);

		const HashMap<StringName, ClassDB::ClassInfo::EnumInfo> &enum_map = class_info->enum_map;

		for (const KeyValue<StringName, ClassDB::ClassInfo::EnumInfo> &E : enum_map) {
			StringName enum_proxy_cname = E.key;
			String enum_proxy_name = pascal_to_pascal_case(enum_proxy_cname.operator String());
			if (itype.find_property_by_proxy_name(enum_proxy_name) || itype.find_method_by_proxy_name(enum_proxy_name) || itype.find_signal_by_proxy_name(enum_proxy_name)) {
				// In case the enum name conflicts with other PascalCase members,
				// we append 'Enum' to the enum name in those cases.
				// We have several conflicts between enums and PascalCase properties.
				enum_proxy_name += "Enum";
				enum_proxy_cname = StringName(enum_proxy_name);
			}
			EnumInterface ienum(enum_proxy_cname, enum_proxy_name, E.value.is_bitfield);
			const List<StringName> &enum_constants = E.value.constants;
			for (const StringName &constant_cname : enum_constants) {
				String constant_name = constant_cname.operator String();
				int64_t *value = class_info->constant_map.getptr(constant_cname);
				ERR_FAIL_NULL_V(value, false);
				constants.erase(constant_name);

				ConstantInterface iconstant(constant_name, snake_to_pascal_case(constant_name, true), *value);

				iconstant.const_doc = nullptr;
				for (int i = 0; i < itype.class_doc->constants.size(); i++) {
					const DocData::ConstantDoc &const_doc = itype.class_doc->constants[i];

					if (const_doc.name == iconstant.name) {
						iconstant.const_doc = &const_doc;
						break;
					}
				}

				if (iconstant.const_doc) {
					iconstant.is_deprecated = iconstant.const_doc->is_deprecated;
					iconstant.deprecation_message = iconstant.const_doc->deprecated_message;

					if (iconstant.is_deprecated && iconstant.deprecation_message.is_empty()) {
						WARN_PRINT("An empty deprecation message is discouraged. Enum member: '" + itype.proxy_name + "." + ienum.proxy_name + "." + iconstant.proxy_name + "'.");
						iconstant.deprecation_message = "This enum member is deprecated.";
					}
				}

				ienum.constants.push_back(iconstant);
			}

			int prefix_length = _determine_enum_prefix(ienum);

			_apply_prefix_to_enum_constants(ienum, prefix_length);

			itype.enums.push_back(ienum);

			TypeInterface enum_itype;
			enum_itype.is_enum = true;
			enum_itype.name = itype.name + "." + String(E.key);
			enum_itype.cname = StringName(enum_itype.name);
			enum_itype.proxy_name = itype.proxy_name + "." + enum_proxy_name;
			TypeInterface::postsetup_enum_type(enum_itype);
			out_enum_types.insert(enum_itype.cname, enum_itype);
		}

		for (const String &constant_name : constants) {
			int64_t *value = class_info->constant_map.getptr(StringName(constant_name));
			ERR_FAIL_NULL_V(value, false);

			String constant_proxy_name = snake_to_pascal_case(constant_name, true);

			if (itype.find_property_by_proxy_name(constant_proxy_name) || itype.find_method_by_proxy_name(constant_proxy_name) || itype.find_signal_by_proxy_name(constant_proxy_name)) {
				// In case the constant name conflicts with other PascalCase members,
				// we append 'Constant' to the constant name in those cases.
				constant_proxy_name += "Constant";
			}

			ConstantInterface iconstant(constant_name, constant_proxy_name, *value);

			iconstant.const_doc = nullptr;
			for (int i = 0; i < itype.class_doc->constants.size(); i++) {
				const DocData::ConstantDoc &const_doc = itype.class_doc->constants[i];

				if (const_doc.name == iconstant.name) {
					iconstant.const_doc = &const_doc;
					break;
				}
			}

			if (iconstant.const_doc) {
				iconstant.is_deprecated = iconstant.const_doc->is_deprecated;
				iconstant.deprecation_message = iconstant.const_doc->deprecated_message;

				if (iconstant.is_deprecated && iconstant.deprecation_message.is_empty()) {
					WARN_PRINT("An empty deprecation message is discouraged. Constant: '" + itype.proxy_name + "." + iconstant.proxy_name + "'.");
					iconstant.deprecation_message = "This constant is deprecated.";
				}
			}

			itype.constants.push_back(iconstant);
		}

		out_obj_types.insert(itype.cname, itype);

		if (itype.is_singleton) {
			// Add singleton instance type.
			itype.proxy_name += CS_SINGLETON_INSTANCE_SUFFIX;
			itype.is_singleton = false;
			itype.is_singleton_instance = true;

			// Remove constants and enums, those will remain in the static class.
			itype.constants.clear();
			itype.enums.clear();

			out_obj_types.insert(itype.name + CS_SINGLETON_INSTANCE_SUFFIX, itype);
		}
	}
	return true;
}

bool RuntimeBindingsGenerator::_method_has_conflicting_signature(const MethodInterface &p_imethod,
		const TypeInterface &p_itype) {
	// Compare p_imethod with all the methods already registered in p_itype.
	for (const MethodInterface &method : p_itype.methods) {
		if (method.proxy_name == p_imethod.proxy_name) {
			if (_method_has_conflicting_signature(p_imethod, method)) {
				return true;
			}
		}
	}

	return false;
}

int RuntimeBindingsGenerator::_determine_enum_prefix(const EnumInterface &p_ienum) {
	CRASH_COND(p_ienum.constants.is_empty());

	const ConstantInterface &front_iconstant = p_ienum.constants.front()->get();
	Vector<String> front_parts = front_iconstant.name.split("_", /* p_allow_empty: */ true);
	int candidate_len = front_parts.size() - 1;

	if (candidate_len == 0) {
		return 0;
	}

	for (const ConstantInterface &iconstant : p_ienum.constants) {
		Vector<String> parts = iconstant.name.split("_", /* p_allow_empty: */ true);

		int i;
		for (i = 0; i < candidate_len && i < parts.size(); i++) {
			if (front_parts[i] != parts[i]) {
				// HARDCODED: Some Flag enums have the prefix 'FLAG_' for everything except 'FLAGS_DEFAULT' (same for 'METHOD_FLAG_' and'METHOD_FLAGS_DEFAULT').
				bool hardcoded_exc = (i == candidate_len - 1 && ((front_parts[i] == "FLAGS" && parts[i] == "FLAG") || (front_parts[i] == "FLAG" && parts[i] == "FLAGS")));
				if (!hardcoded_exc) {
					break;
				}
			}
		}
		candidate_len = i;

		if (candidate_len == 0) {
			return 0;
		}
	}

	return candidate_len;
}

bool RuntimeBindingsGenerator::_method_has_conflicting_signature(const MethodInterface &p_imethod_left,
		const MethodInterface &p_imethod_right) {
	// Check if a method already exists in p_itype with a method signature that would conflict with p_imethod.
	// The return type is ignored because only changing the return type is not enough to avoid conflicts.
	// The const keyword is also ignored since it doesn't generate different C# code.

	if (p_imethod_left.arguments.size() != p_imethod_right.arguments.size()) {
		// Different argument count, so no conflict.
		return false;
	}

	List<ArgumentInterface>::ConstIterator left_itr = p_imethod_left.arguments.begin();
	List<ArgumentInterface>::ConstIterator right_itr = p_imethod_right.arguments.begin();
	for (; left_itr != p_imethod_left.arguments.end(); ++left_itr, ++right_itr) {
		const ArgumentInterface &iarg_left = *left_itr;
		const ArgumentInterface &iarg_right = *right_itr;

		if (iarg_left.type.cname != iarg_right.type.cname) {
			// Different types for arguments in the same position, so no conflict.
			return false;
		}

		if (iarg_left.def_param_mode != iarg_right.def_param_mode) {
			// If the argument is a value type and nullable, it will be 'Nullable<T>' instead of 'T'
			// and will not create a conflict.
			if (iarg_left.def_param_mode == ArgumentInterface::NULLABLE_VAL || iarg_right.def_param_mode == ArgumentInterface::NULLABLE_VAL) {
				return false;
			}
		}
	}

	return true;
}

void RuntimeBindingsGenerator::_initialize_type_interface(const StringName &type_cname, TypeInterface &out_itype) {
	ClassDB::ClassInfo *class_info = ClassDB::classes.getptr(type_cname);
	out_itype.base_name = ClassDB::get_parent_class(type_cname);
	out_itype.is_singleton = Engine::get_singleton()->has_singleton(type_cname);
	out_itype.is_instantiable = class_info->creation_func && !out_itype.is_singleton;
	out_itype.is_ref_counted = ClassDB::is_parent_class(type_cname, name_cache.type_RefCounted);
	out_itype.memory_own = out_itype.is_ref_counted;

	if (out_itype.class_doc) {
		out_itype.is_deprecated = out_itype.class_doc->is_deprecated;
		out_itype.deprecation_message = out_itype.class_doc->deprecated_message;

		if (out_itype.is_deprecated && out_itype.deprecation_message.is_empty()) {
			WARN_PRINT("An empty deprecation message is discouraged. Type: '" +
					out_itype.proxy_name + "'.");
			out_itype.deprecation_message = "This class is deprecated.";
		}
	}

	if (out_itype.is_singleton && compat_singletons.has(out_itype.cname)) {
		out_itype.is_singleton = false;
		out_itype.is_compat_singleton = true;
	}

	out_itype.c_out = "%5return ";
	out_itype.c_out += C_METHOD_UNMANAGED_GET_MANAGED;
	out_itype.c_out += out_itype.is_ref_counted ? "(%1.Reference);\n" : "(%1);\n";

	out_itype.cs_type = out_itype.proxy_name;
	out_itype.cs_in_expr = "GodotObject." CS_STATIC_METHOD_GETINSTANCE "(%0)";
	out_itype.cs_out = "%5return (%2)%0(%1);";

	out_itype.c_arg_in = "&%s";
	out_itype.c_type = "IntPtr";
	out_itype.c_type_in = out_itype.c_type;
	out_itype.c_type_out = "GodotObject";
}

Error RuntimeBindingsGenerator::_populate_property_interface(
		const PropertyInfo &property, const StringName &type_cname, const TypeInterface &itype,
		HashMap<StringName, StringName> &accessor_methods, PropertyInterface &out_iprop) {
	out_iprop.cname = property.name;
	out_iprop.setter = ClassDB::get_property_setter(type_cname, out_iprop.cname);
	out_iprop.getter = ClassDB::get_property_getter(type_cname, out_iprop.cname);

	// If the property is internal hide it; otherwise, hide the getter and setter.
	if (property.usage & PROPERTY_USAGE_INTERNAL) {
		out_iprop.is_hidden = true;
	} else {
		if (out_iprop.setter != StringName()) {
			accessor_methods[out_iprop.setter] = out_iprop.cname;
		}
		if (out_iprop.getter != StringName()) {
			accessor_methods[out_iprop.getter] = out_iprop.cname;
		}
	}

	bool valid = false;
	out_iprop.index = ClassDB::get_property_index(type_cname, out_iprop.cname, &valid);
	ERR_FAIL_COND_V_MSG(!valid, ERR_INVALID_DATA, "Invalid property: '" + itype.name + "." + String(out_iprop.cname) + "'.");

	out_iprop.proxy_name = escape_csharp_keyword(snake_to_pascal_case(out_iprop.cname));

	// Prevent the property and its enclosing type from sharing the same name
	if (out_iprop.proxy_name == itype.proxy_name) {
		_log("Name of property '%s' is ambiguous with the name of its enclosing class '%s'. Renaming property to '%s_'\n",
				out_iprop.proxy_name.utf8().get_data(), itype.proxy_name.utf8().get_data(), out_iprop.proxy_name.utf8().get_data());

		out_iprop.proxy_name += "_";
	}

	out_iprop.prop_doc = nullptr;

	for (int i = 0; i < itype.class_doc->properties.size(); i++) {
		const DocData::PropertyDoc &prop_doc = itype.class_doc->properties[i];

		if (prop_doc.name == out_iprop.cname) {
			out_iprop.prop_doc = &prop_doc;
			break;
		}
	}

	if (out_iprop.prop_doc) {
		out_iprop.is_deprecated = out_iprop.prop_doc->is_deprecated;
		out_iprop.deprecation_message = out_iprop.prop_doc->deprecated_message;

		if (out_iprop.is_deprecated && out_iprop.deprecation_message.is_empty()) {
			WARN_PRINT("An empty deprecation message is discouraged. Property: '" + itype.proxy_name + "." + out_iprop.proxy_name + "'.");
			out_iprop.deprecation_message = "This property is deprecated.";
		}
	}

	return OK;
}


StringName RuntimeBindingsGenerator::_get_int_type_name_from_meta(GodotTypeInfo::Metadata p_meta) {
	switch (p_meta) {
		case GodotTypeInfo::METADATA_INT_IS_INT8:
			return "sbyte";
			break;
		case GodotTypeInfo::METADATA_INT_IS_INT16:
			return "short";
			break;
		case GodotTypeInfo::METADATA_INT_IS_INT32:
			return "int";
			break;
		case GodotTypeInfo::METADATA_INT_IS_INT64:
			return "long";
			break;
		case GodotTypeInfo::METADATA_INT_IS_UINT8:
			return "byte";
			break;
		case GodotTypeInfo::METADATA_INT_IS_UINT16:
			return "ushort";
			break;
		case GodotTypeInfo::METADATA_INT_IS_UINT32:
			return "uint";
			break;
		case GodotTypeInfo::METADATA_INT_IS_UINT64:
			return "ulong";
			break;
		case GodotTypeInfo::METADATA_INT_IS_CHAR16:
			return "char";
			break;
		case GodotTypeInfo::METADATA_INT_IS_CHAR32:
			// To prevent breaking compatibility, C# bindings need to keep using `long`.
			return "long";
		default:
			// Assume INT64
			return "long";
	}
}

StringName RuntimeBindingsGenerator::_get_float_type_name_from_meta(GodotTypeInfo::Metadata p_meta) {
	switch (p_meta) {
		case GodotTypeInfo::METADATA_REAL_IS_FLOAT:
			return "float";
			break;
		case GodotTypeInfo::METADATA_REAL_IS_DOUBLE:
			return "double";
			break;
		default:
			// Assume FLOAT64
			return "double";
	}
}

StringName RuntimeBindingsGenerator::_get_type_name_from_meta(Variant::Type p_type, GodotTypeInfo::Metadata p_meta) {
	if (p_type == Variant::INT) {
		return _get_int_type_name_from_meta(p_meta);
	} else if (p_type == Variant::FLOAT) {
		return _get_float_type_name_from_meta(p_meta);
	} else {
		return Variant::get_type_name(p_type);
	}
}

bool RuntimeBindingsGenerator::_arg_default_value_from_variant(const Variant &p_val, ArgumentInterface &r_iarg) {
	r_iarg.def_param_value = p_val;

	switch (p_val.get_type()) {
		case Variant::NIL:
			// Either Object type or Variant
			r_iarg.default_argument = "default";
			break;
		// Atomic types
		case Variant::BOOL:
			r_iarg.default_argument = bool(p_val) ? "true" : "false";
			break;
		case Variant::INT:
			if (r_iarg.type.cname != name_cache.type_int) {
				r_iarg.default_argument = "(%s)(" + p_val.operator String() + ")";
			} else {
				r_iarg.default_argument = p_val.operator String();
			}
			break;
		case Variant::FLOAT:
			r_iarg.default_argument = p_val.operator String();

			if (r_iarg.type.cname == name_cache.type_float) {
				r_iarg.default_argument += "f";
			}
			break;
		case Variant::STRING:
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			if (r_iarg.type.cname == name_cache.type_StringName || r_iarg.type.cname == name_cache.type_NodePath) {
				if (r_iarg.default_argument.length() > 0) {
					r_iarg.default_argument = "(%s)\"" + p_val.operator String() + "\"";
					r_iarg.def_param_mode = ArgumentInterface::NULLABLE_REF;
				} else {
					// No need for a special `in` statement to change `null` to `""`. Marshaling takes care of this already.
					r_iarg.default_argument = "null";
				}
			} else {
				CRASH_COND(r_iarg.type.cname != name_cache.type_String);
				r_iarg.default_argument = "\"" + p_val.operator String() + "\"";
			}
			break;
		case Variant::PLANE: {
			Plane plane = p_val.operator Plane();
			r_iarg.default_argument = "new Plane(new Vector3(" +
					_get_vector3_cs_ctor_args(plane.normal) + "), " + rtos(plane.d) + "f)";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::AABB: {
			AABB aabb = p_val.operator ::AABB();
			r_iarg.default_argument = "new Aabb(new Vector3(" +
					_get_vector3_cs_ctor_args(aabb.position) + "), new Vector3(" +
					_get_vector3_cs_ctor_args(aabb.size) + "))";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::RECT2: {
			Rect2 rect = p_val.operator Rect2();
			r_iarg.default_argument = "new Rect2(new Vector2(" +
					_get_vector2_cs_ctor_args(rect.position) + "), new Vector2(" +
					_get_vector2_cs_ctor_args(rect.size) + "))";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::RECT2I: {
			Rect2i rect = p_val.operator Rect2i();
			r_iarg.default_argument = "new Rect2I(new Vector2I(" +
					_get_vector2i_cs_ctor_args(rect.position) + "), new Vector2I(" +
					_get_vector2i_cs_ctor_args(rect.size) + "))";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::COLOR:
			r_iarg.default_argument = "new Color(" + _get_color_cs_ctor_args(p_val.operator Color()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::VECTOR2:
			r_iarg.default_argument = "new Vector2(" + _get_vector2_cs_ctor_args(p_val.operator Vector2()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::VECTOR2I:
			r_iarg.default_argument = "new Vector2I(" + _get_vector2i_cs_ctor_args(p_val.operator Vector2i()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::VECTOR3:
			r_iarg.default_argument = "new Vector3(" + _get_vector3_cs_ctor_args(p_val.operator Vector3()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::VECTOR3I:
			r_iarg.default_argument = "new Vector3I(" + _get_vector3i_cs_ctor_args(p_val.operator Vector3i()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::VECTOR4:
			r_iarg.default_argument = "new Vector4(" + _get_vector4_cs_ctor_args(p_val.operator Vector4()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::VECTOR4I:
			r_iarg.default_argument = "new Vector4I(" + _get_vector4i_cs_ctor_args(p_val.operator Vector4i()) + ")";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
			break;
		case Variant::OBJECT:
			ERR_FAIL_COND_V_MSG(!p_val.is_zero(), false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' can only have null/zero as the default value.");

			r_iarg.default_argument = "null";
			break;
		case Variant::DICTIONARY:
			ERR_FAIL_COND_V_MSG(!p_val.operator Dictionary().is_empty(), false,
					"Default value of type 'Dictionary' must be an empty dictionary.");
			// The [cs_in] expression already interprets null values as empty dictionaries.
			r_iarg.default_argument = "null";
			r_iarg.def_param_mode = ArgumentInterface::CONSTANT;
			break;
		case Variant::RID:
			ERR_FAIL_COND_V_MSG(r_iarg.type.cname != name_cache.type_RID, false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' cannot have a default value of type '" + String(name_cache.type_RID) + "'.");

			ERR_FAIL_COND_V_MSG(!p_val.is_zero(), false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' can only have null/zero as the default value.");

			r_iarg.default_argument = "default";
			break;
		case Variant::ARRAY:
			ERR_FAIL_COND_V_MSG(!p_val.operator Array().is_empty(), false,
					"Default value of type 'Array' must be an empty array.");
			// The [cs_in] expression already interprets null values as empty arrays.
			r_iarg.default_argument = "null";
			r_iarg.def_param_mode = ArgumentInterface::CONSTANT;
			break;
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
			r_iarg.default_argument = "Array.Empty<%s>()";
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_REF;
			break;
		case Variant::TRANSFORM2D: {
			Transform2D transform = p_val.operator Transform2D();
			if (transform == Transform2D()) {
				r_iarg.default_argument = "Transform2D.Identity";
			} else {
				r_iarg.default_argument = "new Transform2D(new Vector2(" +
						_get_vector2_cs_ctor_args(transform.columns[0]) + "), new Vector2(" +
						_get_vector2_cs_ctor_args(transform.columns[1]) + "), new Vector2(" +
						_get_vector2_cs_ctor_args(transform.columns[2]) + "))";
			}
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::TRANSFORM3D: {
			Transform3D transform = p_val.operator Transform3D();
			if (transform == Transform3D()) {
				r_iarg.default_argument = "Transform3D.Identity";
			} else {
				Basis basis = transform.basis;
				r_iarg.default_argument = "new Transform3D(new Vector3(" +
						_get_vector3_cs_ctor_args(basis.get_column(0)) + "), new Vector3(" +
						_get_vector3_cs_ctor_args(basis.get_column(1)) + "), new Vector3(" +
						_get_vector3_cs_ctor_args(basis.get_column(2)) + "), new Vector3(" +
						_get_vector3_cs_ctor_args(transform.origin) + "))";
			}
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::PROJECTION: {
			Projection projection = p_val.operator Projection();
			if (projection == Projection()) {
				r_iarg.default_argument = "Projection.Identity";
			} else {
				r_iarg.default_argument = "new Projection(new Vector4(" +
						_get_vector4_cs_ctor_args(projection.columns[0]) + "), new Vector4(" +
						_get_vector4_cs_ctor_args(projection.columns[1]) + "), new Vector4(" +
						_get_vector4_cs_ctor_args(projection.columns[2]) + "), new Vector4(" +
						_get_vector4_cs_ctor_args(projection.columns[3]) + "))";
			}
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::BASIS: {
			Basis basis = p_val.operator Basis();
			if (basis == Basis()) {
				r_iarg.default_argument = "Basis.Identity";
			} else {
				r_iarg.default_argument = "new Basis(new Vector3(" +
						_get_vector3_cs_ctor_args(basis.get_column(0)) + "), new Vector3(" +
						_get_vector3_cs_ctor_args(basis.get_column(1)) + "), new Vector3(" +
						_get_vector3_cs_ctor_args(basis.get_column(2)) + "))";
			}
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::QUATERNION: {
			Quaternion quaternion = p_val.operator Quaternion();
			if (quaternion == Quaternion()) {
				r_iarg.default_argument = "Quaternion.Identity";
			} else {
				r_iarg.default_argument = "new Quaternion(" +
						String::num_real(quaternion.x, false) + "f, " +
						String::num_real(quaternion.y, false) + "f, " +
						String::num_real(quaternion.z, false) + "f, " +
						String::num_real(quaternion.w, false) + "f)";
			}
			r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
		} break;
		case Variant::CALLABLE:
			ERR_FAIL_COND_V_MSG(r_iarg.type.cname != name_cache.type_Callable, false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' cannot have a default value of type '" + String(name_cache.type_Callable) + "'.");
			ERR_FAIL_COND_V_MSG(!p_val.is_zero(), false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' can only have null/zero as the default value.");
			r_iarg.default_argument = "default";
			break;
		case Variant::SIGNAL:
			ERR_FAIL_COND_V_MSG(r_iarg.type.cname != name_cache.type_Signal, false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' cannot have a default value of type '" + String(name_cache.type_Signal) + "'.");
			ERR_FAIL_COND_V_MSG(!p_val.is_zero(), false,
					"Parameter of type '" + String(r_iarg.type.cname) +
							"' can only have null/zero as the default value.");
			r_iarg.default_argument = "default";
			break;
		case Variant::VARIANT_MAX:
			ERR_FAIL_V_MSG(false, "Unexpected Variant type: " + itos(p_val.get_type()));
			break;
	}

	if (r_iarg.def_param_mode == ArgumentInterface::CONSTANT && r_iarg.type.cname == name_cache.type_Variant && r_iarg.default_argument != "default") {
		r_iarg.def_param_mode = ArgumentInterface::NULLABLE_VAL;
	}

	return true;
}

void RuntimeBindingsGenerator::_apply_prefix_to_enum_constants(EnumInterface &p_ienum, int p_prefix_length) {
	if (p_prefix_length > 0) {
		for (ConstantInterface &iconstant : p_ienum.constants) {
			int curr_prefix_length = p_prefix_length;

			String constant_name = iconstant.name;

			Vector<String> parts = constant_name.split("_", /* p_allow_empty: */ true);

			if (parts.size() <= curr_prefix_length) {
				continue;
			}

			if (is_digit(parts[curr_prefix_length][0])) {
				// The name of enum constants may begin with a numeric digit when strip from the enum prefix,
				// so we make the prefix for this constant one word shorter in those cases.
				for (curr_prefix_length = curr_prefix_length - 1; curr_prefix_length > 0; curr_prefix_length--) {
					if (!is_digit(parts[curr_prefix_length][0])) {
						break;
					}
				}
			}

			constant_name = "";
			for (int i = curr_prefix_length; i < parts.size(); i++) {
				if (i > curr_prefix_length) {
					constant_name += "_";
				}
				constant_name += parts[i];
			}

			iconstant.proxy_name = snake_to_pascal_case(constant_name, true);
		}
	}
}
