file(GLOB_RECURSE SHADER_GLSL_SRCS "*.glsl")

# HZM [2026-11-01] Shaders are embedded by a CMake script, not a compiled helper: a helper built with the target
# compiler cannot run on a cross-compiling host (Linux CI builds linux-amd64 on an ARM runner). See stringify.cmake.
set(HZM_STRINGIFY "${CMAKE_CURRENT_LIST_DIR}/stringify.cmake")

foreach (shader ${SHADER_GLSL_SRCS})
	get_filename_component(shaderfile ${shader} NAME_WE)
	list(APPEND SHADER_SRCS ${CMAKE_CURRENT_BINARY_DIR}/${shaderfile}.c)
	set_source_files_properties(${shaderfile}.c PROPERTIES GENERATED TRUE)
	add_custom_command(OUTPUT ${shaderfile}.c
		DEPENDS ${shader} ${HZM_STRINGIFY}
		COMMAND ${CMAKE_COMMAND} -DIN=${shader} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/${shaderfile}.c -P ${HZM_STRINGIFY}
		COMMENT "Generate c source for ${shader}: ${shaderfile}.c"
	)
endforeach()
