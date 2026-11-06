/*
 * PROJECT:     ReactOS msvcrt.dll
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     ARM64EC C++ MSVC aliases for cpp.c
 * COPYRIGHT:   Copyright 2014-2024 Wine team
 *              Copyright 2025 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Justin Miller <justin.miller@reactos.org>
 */

#include <kxarm64.h>

// The x64 names of the exports, and the ARM64EC names ($$h) the C++ compiler uses for them

    IMPORT |??0__non_rtti_object@@QEAA@AEBV0@@Z|, WEAK |#__non_rtti_object_copy_ctor|
    IMPORT |??0__non_rtti_object@@$$hQEAA@AEBV0@@Z|, WEAK |#__non_rtti_object_copy_ctor|
    IMPORT |??0__non_rtti_object@@QEAA@PEBD@Z|, WEAK |#__non_rtti_object_ctor|
    IMPORT |??0__non_rtti_object@@$$hQEAA@PEBD@Z|, WEAK |#__non_rtti_object_ctor|
    IMPORT |??0bad_cast@@AEAA@PEBQEBD@Z|, WEAK |#bad_cast_ctor|
    IMPORT |??0bad_cast@@$$hAEAA@PEBQEBD@Z|, WEAK |#bad_cast_ctor|
    IMPORT |??0bad_cast@@QEAA@AEBQEBD@Z|, WEAK |#bad_cast_ctor|
    IMPORT |??0bad_cast@@$$hQEAA@AEBQEBD@Z|, WEAK |#bad_cast_ctor|
    IMPORT |??0bad_cast@@QEAA@AEBV0@@Z|, WEAK |#bad_cast_copy_ctor|
    IMPORT |??0bad_cast@@$$hQEAA@AEBV0@@Z|, WEAK |#bad_cast_copy_ctor|
    IMPORT |??0bad_cast@@QEAA@PEBD@Z|, WEAK |#bad_cast_ctor_charptr|
    IMPORT |??0bad_cast@@$$hQEAA@PEBD@Z|, WEAK |#bad_cast_ctor_charptr|
    IMPORT |??0bad_typeid@@QEAA@AEBV0@@Z|, WEAK |#bad_typeid_copy_ctor|
    IMPORT |??0bad_typeid@@$$hQEAA@AEBV0@@Z|, WEAK |#bad_typeid_copy_ctor|
    IMPORT |??0bad_typeid@@QEAA@PEBD@Z|, WEAK |#bad_typeid_ctor|
    IMPORT |??0bad_typeid@@$$hQEAA@PEBD@Z|, WEAK |#bad_typeid_ctor|
    IMPORT |??0exception@@QEAA@AEBQEBD@Z|, WEAK |#exception_ctor|
    IMPORT |??0exception@@$$hQEAA@AEBQEBD@Z|, WEAK |#exception_ctor|
    IMPORT |??0exception@@QEAA@AEBQEBDH@Z|, WEAK |#exception_ctor_noalloc|
    IMPORT |??0exception@@$$hQEAA@AEBQEBDH@Z|, WEAK |#exception_ctor_noalloc|
    IMPORT |??0exception@@QEAA@AEBV0@@Z|, WEAK |#exception_copy_ctor|
    IMPORT |??0exception@@$$hQEAA@AEBV0@@Z|, WEAK |#exception_copy_ctor|
    IMPORT |??0exception@@QEAA@XZ|, WEAK |#exception_default_ctor|
    IMPORT |??0exception@@$$hQEAA@XZ|, WEAK |#exception_default_ctor|
    IMPORT |??1__non_rtti_object@@UEAA@XZ|, WEAK |#__non_rtti_object_dtor|
    IMPORT |??1__non_rtti_object@@$$hUEAA@XZ|, WEAK |#__non_rtti_object_dtor|
    IMPORT |??1bad_cast@@UEAA@XZ|, WEAK |#bad_cast_dtor|
    IMPORT |??1bad_cast@@$$hUEAA@XZ|, WEAK |#bad_cast_dtor|
    IMPORT |??1bad_typeid@@UEAA@XZ|, WEAK |#bad_typeid_dtor|
    IMPORT |??1bad_typeid@@$$hUEAA@XZ|, WEAK |#bad_typeid_dtor|
    IMPORT |??1exception@@UEAA@XZ|, WEAK |#exception_dtor|
    IMPORT |??1exception@@$$hUEAA@XZ|, WEAK |#exception_dtor|
    IMPORT |??1type_info@@UEAA@XZ|, WEAK |#type_info_dtor|
    IMPORT |??1type_info@@$$hUEAA@XZ|, WEAK |#type_info_dtor|
    IMPORT |??2@YAPEAX_K@Z|, WEAK |#operator_new|
    IMPORT |??2@$$hYAPEAX_K@Z|, WEAK |#operator_new|
    IMPORT |??2@YAPEAX_KHPEBDH@Z|, WEAK |#operator_new_dbg|
    IMPORT |??2@$$hYAPEAX_KHPEBDH@Z|, WEAK |#operator_new_dbg|
    IMPORT |??3@YAXPEAX@Z|, WEAK |#operator_delete|
    IMPORT |??3@$$hYAXPEAX@Z|, WEAK |#operator_delete|
    IMPORT |??4__non_rtti_object@@QEAAAEAV0@AEBV0@@Z|, WEAK |#__non_rtti_object_opequals|
    IMPORT |??4__non_rtti_object@@$$hQEAAAEAV0@AEBV0@@Z|, WEAK |#__non_rtti_object_opequals|
    IMPORT |??4bad_cast@@QEAAAEAV0@AEBV0@@Z|, WEAK |#bad_cast_opequals|
    IMPORT |??4bad_cast@@$$hQEAAAEAV0@AEBV0@@Z|, WEAK |#bad_cast_opequals|
    IMPORT |??4bad_typeid@@QEAAAEAV0@AEBV0@@Z|, WEAK |#bad_typeid_opequals|
    IMPORT |??4bad_typeid@@$$hQEAAAEAV0@AEBV0@@Z|, WEAK |#bad_typeid_opequals|
    IMPORT |??4exception@@QEAAAEAV0@AEBV0@@Z|, WEAK |#exception_opequals|
    IMPORT |??4exception@@$$hQEAAAEAV0@AEBV0@@Z|, WEAK |#exception_opequals|
    IMPORT |??8type_info@@QEBAHAEBV0@@Z|, WEAK |#type_info_opequals_equals|
    IMPORT |??8type_info@@$$hQEBAHAEBV0@@Z|, WEAK |#type_info_opequals_equals|
    IMPORT |??9type_info@@QEBAHAEBV0@@Z|, WEAK |#type_info_opnot_equals|
    IMPORT |??9type_info@@$$hQEBAHAEBV0@@Z|, WEAK |#type_info_opnot_equals|
    IMPORT |??_Fbad_cast@@QEAAXXZ|, WEAK |#bad_cast_default_ctor|
    IMPORT |??_Fbad_cast@@$$hQEAAXXZ|, WEAK |#bad_cast_default_ctor|
    IMPORT |??_Fbad_typeid@@QEAAXXZ|, WEAK |#bad_typeid_default_ctor|
    IMPORT |??_Fbad_typeid@@$$hQEAAXXZ|, WEAK |#bad_typeid_default_ctor|
    IMPORT |??_U@YAPEAX_K@Z|, WEAK |#operator_new|
    IMPORT |??_U@$$hYAPEAX_K@Z|, WEAK |#operator_new|
    IMPORT |??_U@YAPEAX_KHPEBDH@Z|, WEAK |#operator_new_dbg|
    IMPORT |??_U@$$hYAPEAX_KHPEBDH@Z|, WEAK |#operator_new_dbg|
    IMPORT |??_V@YAXPEAX@Z|, WEAK |#operator_delete|
    IMPORT |??_V@$$hYAXPEAX@Z|, WEAK |#operator_delete|
    IMPORT |?_query_new_handler@@YAP6AH_K@ZXZ|, WEAK |#_query_new_handler|
    IMPORT |?_query_new_handler@@$$hYAP6AH_K@ZXZ|, WEAK |#_query_new_handler|
    IMPORT |?_query_new_mode@@YAHXZ|, WEAK |#_query_new_mode|
    IMPORT |?_query_new_mode@@$$hYAHXZ|, WEAK |#_query_new_mode|
    IMPORT |?_set_new_handler@@YAP6AH_K@ZP6AH0@Z@Z|, WEAK |#_set_new_handler|
    IMPORT |?_set_new_handler@@$$hYAP6AH_K@ZP6AH0@Z@Z|, WEAK |#_set_new_handler|
    IMPORT |?_set_new_mode@@YAHH@Z|, WEAK |#_set_new_mode|
    IMPORT |?_set_new_mode@@$$hYAHH@Z|, WEAK |#_set_new_mode|
    IMPORT |?_set_se_translator@@YAP6AXIPEAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z|, WEAK |#_set_se_translator|
    IMPORT |?_set_se_translator@@$$hYAP6AXIPEAU_EXCEPTION_POINTERS@@@ZP6AXI0@Z@Z|, WEAK |#_set_se_translator|
    IMPORT |?before@type_info@@QEBAHAEBV1@@Z|, WEAK |#type_info_before|
    IMPORT |?before@type_info@@$$hQEBAHAEBV1@@Z|, WEAK |#type_info_before|
    IMPORT |?name@type_info@@QEBAPEBDXZ|, WEAK |#type_info_name|
    IMPORT |?name@type_info@@$$hQEBAPEBDXZ|, WEAK |#type_info_name|
    IMPORT |?raw_name@type_info@@QEBAPEBDXZ|, WEAK |#type_info_raw_name|
    IMPORT |?raw_name@type_info@@$$hQEBAPEBDXZ|, WEAK |#type_info_raw_name|
    IMPORT |?set_new_handler@@YAP6AXXZP6AXXZ@Z|, WEAK |#set_new_handler|
    IMPORT |?set_new_handler@@$$hYAP6AXXZP6AXXZ@Z|, WEAK |#set_new_handler|
    IMPORT |?set_terminate@@YAP6AXXZP6AXXZ@Z|, WEAK |#set_terminate|
    IMPORT |?set_terminate@@$$hYAP6AXXZP6AXXZ@Z|, WEAK |#set_terminate|
    IMPORT |?set_unexpected@@YAP6AXXZP6AXXZ@Z|, WEAK |#set_unexpected|
    IMPORT |?set_unexpected@@$$hYAP6AXXZP6AXXZ@Z|, WEAK |#set_unexpected|
    IMPORT |?terminate@@YAXXZ|, WEAK |#terminate|
    IMPORT |?terminate@@$$hYAXXZ|, WEAK |#terminate|
    IMPORT |?unexpected@@YAXXZ|, WEAK |#unexpected|
    IMPORT |?unexpected@@$$hYAXXZ|, WEAK |#unexpected|
    IMPORT |?what@exception@@UEBAPEBDXZ|, WEAK |#exception_what|
    IMPORT |?what@exception@@$$hUEBAPEBDXZ|, WEAK |#exception_what|

    END
