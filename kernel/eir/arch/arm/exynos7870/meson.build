exynos7870_sources = files('exynos7870.S', 'exynos7870.cpp')
objcopy = find_program('aarch64-managarm-objcopy')

exe = executable('eir-exynos7870', eir_sources, exynos7870_sources,
	include_directories : eir_includes,
	cpp_args : [eir_cpp_args, '-mno-unaligned-access'],
	link_args : [eir_link_args, '-Wl,-T,' + meson.current_source_dir() + '/link.x'],
	dependencies : eir_dependencies,
	link_depends : files('link.x'),
	install : true,
)

flat = custom_target('eir-exynos7870.img',
	input : exe,
	output : 'eir-exynos7870.img',
	build_by_default : true,
	command : [ objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@' ]
)

boot_img = custom_target('boot.img',
	command : [
		'mkbootimg',
		'--kernel', '@INPUT@',
		'--pagesize', '2048',
		'--base', '0x40000000',
		'--kernel_offset', '0x80000',
		'-o', '@OUTPUT@'
	],
	input : flat,
	output : 'boot.img',
	build_by_default : true
)
