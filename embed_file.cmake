# Function to convert text file to C++ header with embedded string
function(embed_text_file INPUT_FILE OUTPUT_HEADER VARIABLE_NAME)
    file(READ ${INPUT_FILE} FILE_CONTENT)

    # escape special characters for C++ string literal
    string(REPLACE "\\" "\\\\" FILE_CONTENT "${FILE_CONTENT}")
    string(REPLACE "\"" "\\\"" FILE_CONTENT "${FILE_CONTENT}")
    string(REPLACE ";" "\;" FILE_CONTENT "${FILE_CONTENT}")
    string(REPLACE "\n" "\\n\"\n\"" FILE_CONTENT "${FILE_CONTENT}")

    # get just the filename for the comment
    get_filename_component(INPUT_FILENAME ${INPUT_FILE} NAME)

    # Get file size
    file(SIZE ${INPUT_FILE} FILE_SIZE)
    set(HEADER_CONTENT
            "// Auto-generated from ${INPUT_FILENAME}
// File size: ${FILE_SIZE} bytes

namespace embedded {
    constexpr const char* ${VARIABLE_NAME} =
\"${FILE_CONTENT}\"\;
}
")

    # Write the header file
    file(WRITE ${OUTPUT_HEADER} ${HEADER_CONTENT})

    message(STATUS "Generated: ${VARIABLE_NAME} (${FILE_SIZE} bytes)")
endfunction()

# Main function to embed OpenCL kernel file
function(embed_opencl_kernel)
    # Parse arguments
    set(options "")
    set(oneValueArgs INPUT_FILE OUTPUT_HEADER VARIABLE_NAME)
    set(multiValueArgs "")
    cmake_parse_arguments(EOK "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # Validate required arguments
    if (NOT EOK_INPUT_FILE)
        message(FATAL_ERROR "INPUT_FILE is required")
    endif ()
    if (NOT EOK_OUTPUT_HEADER)
        message(FATAL_ERROR "OUTPUT_HEADER is required")
    endif ()
    if (NOT EOK_VARIABLE_NAME)
        message(FATAL_ERROR "VARIABLE_NAME is required")
    endif ()

    # Get the directory of the output file
    get_filename_component(OUTPUT_DIR ${EOK_OUTPUT_HEADER} DIRECTORY)

    # Create output directory if it doesn't exist
    file(MAKE_DIRECTORY ${OUTPUT_DIR})

    # Check if input file exists
    if (NOT EXISTS ${EOK_INPUT_FILE})
        message(FATAL_ERROR "Input file does not exist: ${EOK_INPUT_FILE}")
    endif ()

    # Convert file to header
    embed_text_file(${EOK_INPUT_FILE} ${EOK_OUTPUT_HEADER} ${EOK_VARIABLE_NAME})
endfunction()