# Laid out under out/ the way it will be laid out once installed, beside bin and lib, so that
# installing it is a copy.
set(_doxy_dir ${CMAKE_CURRENT_BINARY_DIR}/out/share/doc/${STDC_INSTALL_NAME})

# doxygen-awesome-css, https://github.com/jothepro/doxygen-awesome-css, MIT. Stock Doxygen output
# looks like stock Doxygen output.
#
# Fetched rather than committed, pinned to a tag and checked against a hash, so that eighty
# kilobytes of somebody else's stylesheet is not carried in this repository. It lands in the
# build directory and is downloaded once: a configure that finds the file already there does
# nothing. The only build that needs the network is one that asked for documentation.
set(_doxy_awesome_tag "v2.4.2")
set(_doxy_awesome_dir ${CMAKE_CURRENT_BINARY_DIR}/doxygen-awesome-css-${_doxy_awesome_tag})
set(_doxy_awesome_url
    "https://raw.githubusercontent.com/jothepro/doxygen-awesome-css/${_doxy_awesome_tag}")

foreach(_item
    "doxygen-awesome.css;5ec49e2dfd097f6b5384e3aae0476eab47748e311fc70e207925f8fcc37477b9"
    "doxygen-awesome-sidebar-only.css;dc7ddd235375b71ecb0af920faa6b925ee9445ac617f3bc962b0b0db97da7b4f"
    "LICENSE;e3da754c3f657cc78594fa2e8a3283665f78c743df2485fa9e498a8973051191"
)
    list(GET _item 0 _name)
    list(GET _item 1 _hash)

    if(EXISTS ${_doxy_awesome_dir}/${_name})
        continue()
    endif()

    message(STATUS "Fetching ${_name} from doxygen-awesome-css ${_doxy_awesome_tag}")
    file(DOWNLOAD "${_doxy_awesome_url}/${_name}" ${_doxy_awesome_dir}/${_name}
        EXPECTED_HASH SHA256=${_hash}
        TLS_VERIFY ON
        STATUS _status
    )
    list(GET _status 0 _code)

    if(NOT _code EQUAL 0)
        list(GET _status 1 _reason)
        file(REMOVE ${_doxy_awesome_dir}/${_name})
        message(FATAL_ERROR "Cannot fetch ${_name} for the documentation theme: ${_reason}")
    endif()
endforeach()

# Only the settings that differ from Doxygen's defaults. Everything left out keeps the default
# of whichever Doxygen runs.
# The name the pages carry, which is the target's in capitals, the way the README writes it.
string(TOUPPER ${PROJECT_NAME} _doxy_title)

set(_doxy_content "PROJECT_NAME           = ${_doxy_title}
PROJECT_NUMBER         = ${PROJECT_VERSION}
PROJECT_BRIEF          = \"${DOXY_DESCRIPTION}\"
OUTPUT_DIRECTORY       = ${_doxy_dir}
HTML_OUTPUT            = html
GENERATE_LATEX         = NO

# The public headers and nothing else. src/ holds the implementation and the private _p.h
# headers.
#
# docs/doxygen holds what is written for this and nothing else, which is the landing page. Every
# component is defined by the header that owns it, so that the prose and what it describes cannot
# drift apart, and that leaves only the page no header owns.
#
# The README is not in here. It answers what a reader arriving at the repository asks, which is
# how to build this and how to link it, and none of that belongs in a reference. Carrying it in
# put a second front page beside the real one.
INPUT                  = ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/docs/doxygen
RECURSIVE              = YES
STRIP_FROM_INC_PATH    = ${CMAKE_CURRENT_SOURCE_DIR}/include
STRIP_FROM_PATH        = ${CMAKE_CURRENT_SOURCE_DIR}

# This library writes no \\brief anywhere. Without this the summary table at the top of every
# page is empty.
JAVADOC_AUTOBRIEF      = YES

# The whole public surface, documented or not. An entity whose name says enough still belongs
# in a reference.
EXTRACT_ALL            = YES
EXTRACT_STATIC         = YES
HIDE_UNDOC_MEMBERS     = NO
SORT_MEMBER_DOCS       = NO

# What is in the headers because it has to be, rather than because anybody reads it: every
# detail namespace, the traits that answer a question about a type, and the bases that exist to
# be inherited from once.
#
# The patterns are split on whitespace, so none of them may contain a space. A pattern written
# to name one template specialization, with the space that a template argument list has in it,
# is read as two patterns, and the second one matched enough to leave the index with nothing in
# it at all. Silently, and with no warning from Doxygen. Hence *enable_if* here, which names the
# same two specializations without a space.
#
# stdc::pimpl holds nothing but its detail namespace, so excluding the contents left the page
# behind with not one entry on it. The namespace goes too.
EXCLUDE_SYMBOLS        = *::detail *::detail::* stdc::pimpl stdc::str::conv* *enable_if* stdc::is_map stdc::has_key_type* stdc::has_mapped_type* stdc::vlarray_base stdc::flag stdc::incompatible_flag stdc::winapi::kernel32 stdc::winapi::user32

# Doxygen does not see the configure step, so the switches are spelled out here. Without them
# the export attribute reads as part of every class name, and the platform headers document
# whichever branch Doxygen guessed at.
ENABLE_PREPROCESSING   = YES
MACRO_EXPANSION        = YES
EXPAND_ONLY_PREDEF     = YES
#
# STDC_ALLOCA is not one Doxygen can work out. vla.h defines it from _MSC_VER or __GNUC__,
# neither of which is set here, so everything under the #ifdef below it was missing from the
# output and nothing said so.
PREDEFINED             = STDC_EXPORT= STDC_DECL_EXPORT= STDC_DECL_IMPORT= STDC_HAS_EXCEPTIONS=1 STDC_ALLOCA(size)= _WIN32=1 DOXYGEN=1

# Undocumented is deliberate here, so it is not worth a warning each time. The rest stay on. A
# \\param naming an argument that no longer exists is worth hearing about.
WARN_IF_UNDOCUMENTED   = NO

# The pages are still written, and then the run reports failure. Markup that does not say what
# it meant to say is a defect in the documentation.
WARN_AS_ERROR          = FAIL_ON_WARNINGS

# Set explicitly, because the default is not the same across Doxygen versions. 1.9.8 on a
# runner went looking for Graphviz and failed the build with exit code 127, where 1.10 here had
# never asked for it. Turning it on means installing graphviz wherever this runs.
HAVE_DOT               = NO

# The sidebar-only variant of the theme fetched above. These four are what it asks for rather
# than a taste of ours: the treeview is where it puts the navigation, and HTML_COLORSTYLE has to
# be LIGHT because the stylesheet does the colors itself from Doxygen 1.9.5 on.
#
# Stylesheets only. Its JavaScript extensions, the dark mode toggle among them, need a custom
# HTML_HEADER, and a header copied out of one Doxygen is a thing to keep in step with every
# other Doxygen that ever builds this. The versions here and on the runner already differ.
GENERATE_TREEVIEW      = YES
DISABLE_INDEX          = NO
FULL_SIDEBAR           = NO
HTML_COLORSTYLE        = LIGHT
HTML_EXTRA_STYLESHEET  = ${_doxy_awesome_dir}/doxygen-awesome.css ${_doxy_awesome_dir}/doxygen-awesome-sidebar-only.css

# Written by doxygen -l at build time and then patched, so that a \\defgroup page is called a
# component here rather than a topic, which is what the README calls them. Generated rather than
# committed for the same reason as the header above. See cmake/doxygen-utils/layout.cmake.
LAYOUT_FILE            = ${CMAKE_CURRENT_BINARY_DIR}/DoxygenLayout.xml
")

set(_doxy_file ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}_Doxyfile)
file(WRITE ${_doxy_file} ${_doxy_content})

# Not part of ALL. Documentation is asked for by name, and Doxygen is slow enough that nobody
# wants it on the path from an edit to a test run.
add_custom_target(${PROJECT_NAME}_docs
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_doxy_dir}

    # The layout this Doxygen would use anyway, with the tab holding the \defgroup pages renamed.
    # Written fresh each time rather than committed, so it cannot fall behind the tool.
    COMMAND ${DOXYGEN_EXECUTABLE} -l ${CMAKE_CURRENT_BINARY_DIR}/DoxygenLayout.xml
    COMMAND ${CMAKE_COMMAND} -DLAYOUT=${CMAKE_CURRENT_BINARY_DIR}/DoxygenLayout.xml
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/doxygen-utils/layout.cmake

    COMMAND ${DOXYGEN_EXECUTABLE} ${_doxy_file}

    # Undoes an escaping that Doxygen 1.17.0 applies to text it then assigns as a text node,
    # which spells every operator in the treeview out in entities. See the script for why, and
    # for why a Doxygen that does not do it is left alone.
    COMMAND ${CMAKE_COMMAND} -DNAVTREE=${_doxy_dir}/html/navtree.js
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/doxygen-utils/navtree.cmake

    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating documentation into ${_doxy_dir}/html"
    VERBATIM
)

# Optional, because the target is outside ALL: an install that was never asked for documentation
# should not fail over its absence.
if(STDC_INSTALL)
    install(DIRECTORY ${_doxy_dir}/ DESTINATION ${CMAKE_INSTALL_DOCDIR} OPTIONAL)
endif()
