# grimvault_embed_sql (<target> <sql_dir> <out_include_dir>)
#
# For each .sql file under <sql_dir>, generates <out_include_dir>/<rel>.inc
# containing the file's content wrapped in a raw string literal:
#
#    R"sql(
#       <file content>
#    )sql"
#
# That .inc can be included directly inside a string-typed initializer:
#
#    constexpr std::string_view sql = R"sql(...)sql";        // generated
#    constexpr std::string_view sql =
#       #include "migrations/0001_init.sql.inc"               // consumer
#       ;
#
# Adds <out_include_dir> as a PRIVATE include directory of <target>, and
# attaches the generated files to <target> as a build-time dependency so they
# regenerate when the .sql files change.
#
function (grimvault_embed_sql target sql_dir out_include_dir)
   file (GLOB_RECURSE sql_files CONFIGURE_DEPENDS "${sql_dir}/*.sql")

   set (generated_files)

   foreach (sql ${sql_files})
      file (RELATIVE_PATH rel "${sql_dir}" "${sql}")
      set (out "${out_include_dir}/${rel}.inc")

      file (READ "${sql}" content)

      # Choose a delimiter unlikely to appear in SQL. "sql" is fine; if a file
      # ever contains the literal `)sql"` we'd need to bump it. Guard for that.
      string (FIND "${content}" ")sql\"" collision)

      if (NOT collision EQUAL -1)
         message (FATAL_ERROR
            "SQL file ${sql} contains literal ')sql\"' which collides with "
            "the raw-string delimiter. Bump the delimiter in EmbedSql.cmake."
         )
      endif ()

      set (wrapped "R\"sql(\n${content}\n)sql\"")

      file (WRITE "${out}.tmp" "${wrapped}")
      configure_file ("${out}.tmp" "${out}" COPYONLY)
      file (REMOVE "${out}.tmp")

      list (APPEND generated_files "${out}")
   endforeach ()

   if (generated_files)
      target_sources (${target} PRIVATE ${generated_files})
      target_include_directories (${target} PRIVATE "${out_include_dir}")
      set_source_files_properties (${generated_files} PROPERTIES
         HEADER_FILE_ONLY TRUE
         GENERATED        TRUE
      )
   endif ()
endfunction ()
