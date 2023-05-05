/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GPL - See COPYING in the top level directory
 * FILE:            hal/halarm/generic/cache.S
 * PURPOSE:         Implements cache clean, invalidate routines for ARM machines
 * PROGRAMMERS:     Copyright (C) 2005 ARM Ltd.
 */
#include <ksarm.h>

    TEXTAREA

    NESTED_ENTRY pArmTranslationTableRegisterSet
    PROLOG_END pArmTranslationTableRegisterSet

    mcr p15, 0, r0, c2, c0, 0
    bx lr

    NESTED_END pArmTranslationTableRegisterSet

    NESTED_ENTRY pArmDomainRegisterSet
    PROLOG_END pArmDomainRegisterSet
    mcr p15, 0, r0, c3, c0, 0
    bx lr
    NESTED_END pArmDomainRegisterSet


    NESTED_ENTRY pArmControlRegisterGet
    PROLOG_END pArmControlRegisterGet
    mrc p15, 0, r0, c1, c0, 0
    bx lr
    NESTED_END pArmControlRegisterGet

    NESTED_ENTRY  pArmControlRegisterSet
    PROLOG_END pArmControlRegisterSet
    mcr p15, 0, r0, c1, c0, 0
    bx lr
    NESTED_END pArmControlRegisterSet
 	END
