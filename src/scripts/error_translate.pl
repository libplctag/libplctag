#!/usr/bin/perl

use strict;

my %codes;
my $code;
my $line;
my $i;
my @parts;

while($line = <>) {
        if($line =~ m/(PLCTAG_ERR_[A-Z_]+)/) {
            $codes{$1} = 1;
        }
}

$i = -1;
foreach $code (sort keys %codes) {
    print "#define $code ($i)\n";
    $i--;
}

foreach $code (sort keys %codes) {
    print "case $code: return \"$code\";\n";
}
