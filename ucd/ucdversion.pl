#!/usr/bin/perl
open(UCDVERSION, '<', 'ucd/version') or die;
$line = <UCDVERSION>;
chomp $line;
close(UCDVERSION);

@v = split(/\./, $line);
open(GV, '>', 'gen/ucdversion.c') or die;
print  GV "#include \"ucd.h\"\n";
print  GV "int unicode_database_version(void)\n";
print  GV "{\n";
printf GV "\treturn 0x%x;\n", ($v[0] << 16)+($v[1] << 8)+$v[2];
print  GV "}\n";
close(GV);
