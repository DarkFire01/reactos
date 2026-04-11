/*
 * MSVC ARM64 C++ vtables for cpp.c (layout matches amd64/cpp_vtables.s).
 */
#include <ksarm64.h>

    RODATAAREA

    IMPORT type_info_rtti
    IMPORT type_info_vector_dtor
    ALIGN 8
    EXPORT type_info_vtable
type_info_vtable
    EXPORT |__dummyname_type_info|
|__dummyname_type_info|
    DCQ type_info_rtti
    DCQ type_info_vector_dtor

    IMPORT exception_rtti
    IMPORT exception_vector_dtor
    IMPORT exception_what
    ALIGN 8
    EXPORT exception_vtable
exception_vtable
    EXPORT |??_7exception@@6B@|
|??_7exception@@6B@|
    DCQ exception_rtti
    DCQ exception_vector_dtor
    DCQ exception_what

    IMPORT bad_typeid_rtti
    IMPORT bad_typeid_vector_dtor
    ALIGN 8
    EXPORT bad_typeid_vtable
bad_typeid_vtable
    EXPORT |??_7bad_typeid@@6B@|
|??_7bad_typeid@@6B@|
    DCQ bad_typeid_rtti
    DCQ bad_typeid_vector_dtor
    DCQ exception_what

    IMPORT bad_cast_rtti
    IMPORT bad_cast_vector_dtor
    ALIGN 8
    EXPORT bad_cast_vtable
bad_cast_vtable
    EXPORT |??_7bad_cast@@6B@|
|??_7bad_cast@@6B@|
    DCQ bad_cast_rtti
    DCQ bad_cast_vector_dtor
    DCQ exception_what

    IMPORT __non_rtti_object_rtti
    IMPORT __non_rtti_object_vector_dtor
    ALIGN 8
    EXPORT __non_rtti_object_vtable
__non_rtti_object_vtable
    EXPORT |??_7__non_rtti_object@@6B@|
|??_7__non_rtti_object@@6B@|
    DCQ __non_rtti_object_rtti
    DCQ __non_rtti_object_vector_dtor
    DCQ exception_what

    END
