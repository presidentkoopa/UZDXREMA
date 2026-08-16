add_pk3(doomxr.pk3 ${CMAKE_CURRENT_SOURCE_DIR}/static)

# NOTE (UZDXREMA merge): upstream 5.0.0 generates static/language.csv at build time from
# libraries/Translation/*.po and then POST_BUILD deletes it from the source tree.
# The fork ships hand-maintained wadsrc/static/language.0 and wadsrc/static/language.csv
# containing ~500 DoomXR/VR-only string keys that do not exist in the upstream .po database,
# so enabling the generator here would overwrite and then delete them.
# Left disabled deliberately until the fork strings are migrated into the .po recipe.
#generate_language_files(doomxr_pk3 static/language.csv ENGINE)

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
