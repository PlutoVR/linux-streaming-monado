# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(
	aux_vive STATIC
	vive_config.h
	vive_config.c
	vive_bindings.c
	vive_bindings.h
	vive_poses.h
	vive_poses.c
	)
target_link_libraries(
	aux_vive
	PRIVATE
		xrt-interfaces
		aux_util
		aux_math
		aux_tracking
		xrt-external-cjson
	)
