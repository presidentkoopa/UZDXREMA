add_pk3(doomxr.pk3 ${CMAKE_CURRENT_SOURCE_DIR}/static)

# UZDXREMA merge: upstream 5.0.0 generates static/language.csv at build time from
# libraries/Translation/*.po, then POST_BUILD deletes it from the source tree. It has
# to stay enabled - it is the only source of the stock engine strings (OPTMNU_*,
# DSPLYMNU_*, ...), and without it every stock menu row renders as its raw key.
#
# The fork's own ~525 DoomXR/VR-only keys do not exist in the .po database, so they
# live in static/language.0 and static/language.1 instead. Those filenames are not
# touched by the generator but still load as LANGUAGE lumps (stringtable.cpp scans
# every lump named LANGUAGE), so fork and upstream strings coexist.
#
# Do NOT put fork strings in static/language.csv - the generator overwrites and then
# deletes that file on every build.
generate_language_files(doomxr_pk3 static/language.csv ENGINE)

# insert appstream data into doomxr.pk3, for release notes
add_custom_target(
	copy_metadata
	COMMAND ${CMAKE_COMMAND} -E copy
		"${CMAKE_CURRENT_SOURCE_DIR}/../src/posix/freedesktop/org.zdoom.UZDoom.metainfo.xml"
		"${CMAKE_CURRENT_SOURCE_DIR}/static/meta.xml"
	COMMAND ${CMAKE_COMMAND} -E copy
		"${CMAKE_CURRENT_SOURCE_DIR}/../CONTRIBUTORS"
		"${CMAKE_CURRENT_SOURCE_DIR}/static/contributors.txt"
)
add_dependencies(doomxr_pk3 copy_metadata)
add_custom_command(
	TARGET doomxr_pk3
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E remove
		"${CMAKE_CURRENT_SOURCE_DIR}/static/meta.xml"
	COMMAND ${CMAKE_COMMAND} -E remove
		"${CMAKE_CURRENT_SOURCE_DIR}/static/contributors.txt"
)
