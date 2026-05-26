# grimvault_sign_target (<target>)
#
# Signs the output binary of <target> using SSL.com eSigner CodeSignTool.
# Only runs when GRIMVAULT_ENABLE_SIGNING is ON (CI release builds).
#
# Required environment variables when signing is enabled:
#    SSL_COM_USERNAME
#    SSL_COM_PASSWORD
#    SSL_COM_CREDENTIAL_ID
#    SSL_COM_TOTP_SECRET
#    CODESIGNTOOL_HOME   (directory containing CodeSignTool.bat)
#
# The actual signing in CI is driven by the hearth workflow
# (release-template-shared/sign-windows). This function is kept available for
# local dry-runs; it deliberately fails closed if any input is missing.
#
function (grimvault_sign_target target)
   if (NOT GRIMVAULT_ENABLE_SIGNING)
      return ()
   endif ()

   if (NOT WIN32)
      message (FATAL_ERROR "Signing requested but build is not Windows.")
   endif ()

   foreach (var SSL_COM_USERNAME SSL_COM_PASSWORD SSL_COM_CREDENTIAL_ID
                SSL_COM_TOTP_SECRET CODESIGNTOOL_HOME)
      if (NOT DEFINED ENV{${var}} OR "$ENV{${var}}" STREQUAL "")
         message (FATAL_ERROR "Signing enabled but env var '${var}' is not set.")
      endif ()
   endforeach ()

   add_custom_command (TARGET ${target} POST_BUILD
      COMMAND "$ENV{CODESIGNTOOL_HOME}\\CodeSignTool.bat"
              sign
              -username       "$ENV{SSL_COM_USERNAME}"
              -password       "$ENV{SSL_COM_PASSWORD}"
              -credential_id  "$ENV{SSL_COM_CREDENTIAL_ID}"
              -totp_secret    "$ENV{SSL_COM_TOTP_SECRET}"
              -input_file_path  "$<TARGET_FILE:${target}>"
              -output_dir_path  "$<TARGET_FILE_DIR:${target}>"
      COMMENT "Code-signing ${target} via SSL.com eSigner"
      VERBATIM
   )

   add_custom_command (TARGET ${target} POST_BUILD
      COMMAND signtool verify /pa /v "$<TARGET_FILE:${target}>"
      COMMENT "Verifying Authenticode signature on ${target}"
      VERBATIM
   )
endfunction ()
