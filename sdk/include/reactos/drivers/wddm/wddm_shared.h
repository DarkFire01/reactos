/*
 * PROJECT:     ReactOS Display Driver Model
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     WDDM internal shared types
 * COPYRIGHT:   Copyright 2023 Justin Miller <justinmiller100@gmail.com>
 */

#pragma once

#define IOCTL_VIDEO_DDI_FUNC_REGISTER \
	CTL_CODE( FILE_DEVICE_VIDEO, 0xF, METHOD_NEITHER, FILE_ANY_ACCESS  )

//TODO: Take the time to figure these out better
#define IOCTL_VIDEO_KMDOD_DDI_REGISTER 0x230047
	//CTL_CODE( FILE_DEVICE_VIDEO, ?, METHOD_NEITHER, FILE_ANY_ACCESS  )

#define IOCTL_VIDEO_CDD_DDI_REGISTER 0x23E05B
	//CTL_CODE( FILE_DEVICE_VIDEO, ?, METHOD_NEITHER, FILE_ANY_ACCESS  )

//FIXME: ReactOS Specific
#define IOCTL_VIDEO_ROS_START_ADAPTER \
	CTL_CODE( FILE_DEVICE_VIDEO, 0xB, METHOD_NEITHER, FILE_ANY_ACCESS  )
