#include "runtime_bindings_generator.h"
#include <core/io/dir_access.h>

void RuntimeBindingsGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("generate_object_type", "class_list", "is_gd_extension"),
			&RuntimeBindingsGenerator::generate_object_type);
}

bool RuntimeBindingsGenerator::generate_object_type(const Array &p_class_list, bool is_gd_extension) {
	HashMap<StringName, TypeInterface> obj_types, enum_types;

	LocalVector<StringName> all_class_list;
	ClassDB::get_class_list(all_class_list);

	bool ok = _populate_object_type_interfaces(all_class_list, obj_types, enum_types);
	if (!ok) {
		ERR_PRINT("Failed to populate object type interfaces");
		return false;
	}

	HashSet<StringName> target_classes;
	for (int i = 0; i < p_class_list.size(); i++) {
		target_classes.insert(StringName(p_class_list[i]));
	}

	String output_base_dir = "res://ExtensionGenerated";
	if (!DirAccess::exists(output_base_dir)) {
		Error make_dir_err = DirAccess::make_dir_recursive_absolute(output_base_dir);
		if (make_dir_err != OK) {
			ERR_PRINT("Failed to create output directory: " + output_base_dir);
			return false;
		}
	}

	List<ConstantInterface> global_constants;
	List<EnumInterface> global_enums;
	HashMap<StringName, TypeInterface> builtin_types;
	HashMap<const MethodInterface *, const InternalCall *> method_icalls_map;

	for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
		const TypeInterface &itype = E.value;

		if (!target_classes.has(itype.name)) {
			continue;
		}

		String output_file = output_base_dir.path_join(itype.proxy_name + ".cs");
		Error err = _generate_cs_type(itype, output_file, obj_types, global_constants,
				global_enums, builtin_types, enum_types, method_icalls_map,is_gd_extension);

		if (err != OK) {
			ERR_PRINT("Failed to generate C# type for: " + itype.name);
			return false;
		}

		_log("Generated C# type: %s\n", itype.name.utf8().get_data());
	}

	return true;
}
