#include "runtime_bindings_generator.h"

bool RuntimeBindingsGenerator::generate_object_type(const Array &p_class_list) {
	HashMap<StringName, TypeInterface> obj_types, enum_types;
	LocalVector<StringName> class_list;
	for (int i = 0; i < p_class_list.size(); i++) {
		class_list.push_back(p_class_list[i]);
	}

	bool ok = _populate_object_type_interfaces(class_list, obj_types, enum_types);
	if (!ok) {
		ERR_PRINT("Failed to populate object type interfaces");
		return false;
	}

	List<ConstantInterface> global_constants;
	List<EnumInterface> global_enums;
	HashMap<StringName, TypeInterface> builtin_types;
	HashMap<const MethodInterface *, const InternalCall *> method_icalls_map;

	// Traverse obj_types and generate C# code for each class
	for (const KeyValue<StringName, TypeInterface> &E : obj_types) {
		const TypeInterface &itype = E.value;
		
		// Generate output file path for this type
		String output_file = String("ExtensionGenerated/") + itype.proxy_name + ".cs";
		
		// Call _generate_cs_type to generate C# code for this class
		Error err = _generate_cs_type(itype, output_file, obj_types, global_constants, 
									  global_enums, builtin_types, enum_types, method_icalls_map);
		
		
		if (err != OK) {
			ERR_PRINT("Failed to generate C# type for: " + itype.name);
			return false;
		}
		
		_log("Generated C# type: %s\n", itype.name.utf8().get_data());
	}

	return true;
}
