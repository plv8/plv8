$version = "2.3.14"

$older_versions = @("1.5.0", "1.5.1", "1.5.2", "1.5.3", "1.5.4", "1.5.5", "1.5.6", "1.5.7", "2.0.0", "2.0.1", "2.0.3", "2.1.0", "2.3.0", "2.3.1", "2.3.2", "2.3.3", "2.3.4", "2.3.5", "2.3.6", "2.3.7", "2.3.8", "2.3.9", "2.3.10", "2.3.11", "2.3.12", "2.3.13")

For ($i=0; $i -lt $older_versions.Length; $i++) {
  $old_version = $older_versions[$i]
  $upgrade = @'
  CREATE OR REPLACE FUNCTION plv8_version ( )
  RETURNS TEXT AS
  $$
  	return "{0}";
  $$ LANGUAGE plv8;


  CREATE OR REPLACE FUNCTION plv8_call_handler() RETURNS language_handler
   AS 'MODULE_PATHNAME' LANGUAGE C;
  CREATE OR REPLACE FUNCTION plv8_inline_handler(internal) RETURNS void
   AS 'MODULE_PATHNAME' LANGUAGE C;
  CREATE OR REPLACE FUNCTION plv8_call_validator(oid) RETURNS void
   AS 'MODULE_PATHNAME' LANGUAGE C;
'@ -f $version
  $filename = "plv8--$old_version--$version.sql"
  $upgrade | out-file -filepath $filename
}

get-content ..\plv8.sql.common | %{$_ -replace "@LANG_NAME@","plv8"} | %{$_ -replace "@PLV8_VERSION@",$version} | out-file -filepath plv8--$version.sql
get-content ..\plv8.sql.common | %{$_ -replace "@LANG_NAME@","plls"} | %{$_ -replace "@PLV8_VERSION@",$version} | out-file -filepath plls--$version.sql
get-content ..\plv8.sql.common | %{$_ -replace "@LANG_NAME@","plcoffee"} | %{$_ -replace "@PLV8_VERSION@",$version} | out-file -filepath plcoffee--$version.sql

get-content plv8.control.common | %{$_ -replace "@LANG_NAME@","JavaScript"} | %{$_ -replace "@PLV8_VERSION@",$version} | out-file -filepath plv8.control
get-content plv8.control.common | %{$_ -replace "@LANG_NAME@","CoffeeScript"} | %{$_ -replace "@PLV8_VERSION@",$version} | out-file -filepath plcoffee.control
get-content plv8.control.common | %{$_ -replace "@LANG_NAME@","LiveScript"} | %{$_ -replace "@PLV8_VERSION@",$version} | out-file -filepath plls.control

get-content ..\plv8_config.h.in | %{$_ -replace "#undef PLV8_VERSION","#define PLV8_VERSION ""$version"""} | out-file -filepath plv8_config.h
