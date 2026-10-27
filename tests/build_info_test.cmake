# cmake -DBUILD_INFO=<cmake/build_info.cmake> -DWORK=<an empty scratch directory> -P build_info_test.cmake
#
# Two clones of one commit get one source digest until one of them has an uncommitted change, or an
# untracked file, in a source directory; an ignored file and a file outside the source directories change nothing.
# The header also records the compiler, sanitizers and platform it is given.

function(git)
    execute_process(COMMAND git ${ARGN} RESULT_VARIABLE failed OUTPUT_QUIET ERROR_VARIABLE error)
    if(failed)
        message(FATAL_ERROR "git ${ARGN}: ${error}")
    endif()
endfunction()

# The source digest build_info.cmake writes for `tree`, after checking the provenance it records as given.
function(digest tree out)
    execute_process(COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${tree} -DOUTPUT=${WORK}/info.hpp -DBUILD_TYPE=Test
                            "-DCOMPILER=Test 1.0" -DSANITIZE=undefined "-DPLATFORM=Test x1" -P ${BUILD_INFO}
                    RESULT_VARIABLE failed)
    file(READ ${WORK}/info.hpp header)
    foreach(expected "build_type = \"Test\"" "build_compiler = \"Test 1.0\"" "build_sanitize = \"undefined\""
                     "build_platform = \"Test x1\"")
        string(FIND "${header}" "${expected}" at)
        if(failed OR at EQUAL -1)
            message(FATAL_ERROR "${tree}: no ${expected} in\n${header}")
        endif()
    endforeach()
    if(NOT header MATCHES "build_source_digest = \"([0-9a-f]+)\"")
        message(FATAL_ERROR "${tree}: no source digest in\n${header}")
    endif()
    string(LENGTH "${CMAKE_MATCH_1}" length)
    if(NOT length EQUAL 64)
        message(FATAL_ERROR "${tree}: a source digest of ${length} digits")
    endif()
    set(${out} ${CMAKE_MATCH_1} PARENT_SCOPE)
endfunction()

function(expect a op b what)
    if(op STREQUAL "==" AND NOT a STREQUAL b)
        message(FATAL_ERROR "${what}: ${a} != ${b}")
    elseif(op STREQUAL "!=" AND a STREQUAL b)
        message(FATAL_ERROR "${what}: both ${a}")
    endif()
endfunction()

file(REMOVE_RECURSE ${WORK})
file(WRITE ${WORK}/a/server/a.cpp "int a;\n")
file(WRITE ${WORK}/a/.gitignore "*.o\n")
git(-C ${WORK}/a init -q)
git(-C ${WORK}/a add -A)
git(-C ${WORK}/a -c user.name=t -c user.email=t@t -c commit.gpgsign=false commit -qm one)
git(clone -q ${WORK}/a ${WORK}/b)

digest(${WORK}/a clean)
digest(${WORK}/b clone)
expect(${clean} == ${clone} "two clones of one commit")

file(WRITE ${WORK}/b/server/a.cpp "int a = 1;\n")
digest(${WORK}/b edited)
expect(${edited} != ${clean} "an uncommitted change")

file(WRITE ${WORK}/b/server/a.cpp "int a;\n")
file(WRITE ${WORK}/b/server/b.cpp "int b;\n")
digest(${WORK}/b untracked)
expect(${untracked} != ${clean} "an untracked file")
expect(${untracked} != ${edited} "an untracked file against an uncommitted change")

file(REMOVE ${WORK}/b/server/b.cpp)
file(WRITE ${WORK}/b/server/b.o "ignored")
file(WRITE ${WORK}/b/notes.txt "outside the source directories")
digest(${WORK}/b unchanged)
expect(${unchanged} == ${clean} "an ignored file and one outside the source directories")

file(REMOVE_RECURSE ${WORK})
