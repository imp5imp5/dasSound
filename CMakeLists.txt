
IF (NOT DAS_SOUND_INCLUDED)
    SET(DAS_SOUND_INCLUDED TRUE)
    MESSAGE(STATUS "dasSound module included.")

	SET(DAS_SOUND_DIR ${CMAKE_SOURCE_DIR}/modules/dasSound)
	SET(SOUND_INCLUDE_DIR ${DAS_SOUND_DIR}/src ${DAS_SOUND_DIR}/miniaudio)

    LIST(APPEND CMAKE_MODULE_PATH ${DAS_SOUND_DIR})

	# libDasModuleSound
	SET(DAS_SOUND_MODULE_SRC
		${DAS_SOUND_DIR}/src/dasSound.h
		${DAS_SOUND_DIR}/src/dasSound.cpp
	)

	ADD_MODULE_LIB(libDasModuleSound)
	ADD_MODULE_CPP(Sound)
	# ADD_MODULE_NATIVE(SOUND_boost)
	ADD_LIBRARY(libDasModuleSound ${DAS_SOUND_MODULE_SRC} ${DAS_SOUND_MODULE_PLATFORM_SRC})
	TARGET_LINK_LIBRARIES(libDasModuleSound ${SOUND_LIBRARIES})
	ADD_DEPENDENCIES(libDasModuleSound libDaScript)
	TARGET_INCLUDE_DIRECTORIES(libDasModuleSound PUBLIC ${SOUND_INCLUDE_DIR})

	SETUP_CPP11(libDasModuleSound)

    ADD_MODULE_DAS(media media dasbox_sound_utils)

	# SOUND AOT
	add_custom_target(SOUND_AOT)
	SET(SOUND_GENERATED_SRC)
	DAS_AOT("${DAS_SOUND_DIR}/media/dasbox_sound_utils.das" SOUND_GENERATED_SRC SOUND_AOT daScript)
  DAS_AOT("${DAS_SOUND_DIR}/examples/sound_example.das" SOUND_GENERATED_SRC SOUND_AOT daScript)

	# daslib
	DAS_AOT("daslib/random.das" SOUND_GENERATED_SRC SOUND_AOT daScript)
	DAS_AOT("daslib/math_boost.das" SOUND_GENERATED_SRC SOUND_AOT daScript)
	SOURCE_GROUP_FILES("aot stub" SOUND_GENERATED_SRC)

	# now, the demo
	SET(SOUND_SRC
		${DAS_SOUND_DIR}/examples/soundAot.cpp
		${DAS_MODULES_NEED_INC}
	)
	SOURCE_GROUP_FILES("main" SOUND_SRC)
	add_executable(soundAot ${SOUND_SRC} ${SOUND_GENERATED_SRC})
	TARGET_LINK_LIBRARIES(soundAot libDaScript libDasModuleUriparser libUriParser Threads::Threads)
	ADD_DEPENDENCIES(soundAot libDaScript libDasModuleUriparser libUriParser)
	ADD_DEPENDENCIES(soundAot SOUND_AOT dasAotStub)
	TARGET_INCLUDE_DIRECTORIES(soundAot PUBLIC ${SOUND_INCLUDE_DIR})
	SETUP_CPP11(soundAot)
	SETUP_LTO(soundAot)
	ADD_EXAMPLE(soundAot)

ENDIF()