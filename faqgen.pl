#!/usr/bin/env perl

# $Header

# Copyright 2001 Richard P. Curnow
# LICENCE

# A script to generate an HTML FAQ page from a text input file.  The input is assumed to consist of the following:
# Lines starting with 'S:'.  These introduce sections.
# Lines starting with 'Q:'.  These are the topics of questions.
# Body text (either as an introduction to the sections, or as answers to the questions.
# The body text is set as pre-formatted.

$| = 1;

@prologue = ();
@epilogue = ();

@sections=();  # section titles
@sect_text=(); # introductory text in sections

@questions=(); # questions in sections
@answers=();   # answers to questions

$sn = -1;
$had_q = 0;

#{{{ Parse input 
while (<>) {
    if (m/\@\@PROLOG/o) {
        while (<>) {
            last if (m/^\@\@ENDPROLOG/);
            push (@prologue, $_);
        }
    } elsif (m/\@\@EPILOG/o) {
        while (<>) {
            last if (m/^\@\@ENDEPILOG/);
            push (@epilogue, $_);
        }
    } elsif (m/^[sS]:[ \t]*(.*)$/) {
        chomp;
        $qn = -1;
        ++$sn;
        $sections[$sn] = &guard($1);
        $sect_text[$sn] = "";
        $questions[$sn] = [ ];
        $answers[$sn] = [ ];
        $had_q = 0;
    } elsif (/^[qQ]:[ \t]*(.*)$/) {
        chomp;
        die unless ($sn >= 0);
        ++$qn;
        $questions[$sn]->[$qn] = &guard($1);
        $had_q = 1;
    } else {
        if ($had_q) {
            if ($qn >= 0) {
                $answers[$sn]->[$qn] .= $_;
            }
        } else {
            if ($sect_text[$sn] ne "" || $_ !~ /^\s*$/) {
                $sect_text[$sn] .= $_;
            }
        }
    }
}
#}}}

# Emit file header
if ($#prologue >= 0) {
    print @prologue;
} else {
print <<EOF;
<html>
<head>
<title>
Chrony Frequently Asked Questions
</title>
</head>
<body>
<font face=\"arial,helvetica\" size=+4><b>Table of contents</b></font>
EOF
}

# Emit table of contents
print "<ul>\n";
for $sn (0 .. $#sections) {
    print "<b><li> <a href=\"#section_".($sn+1)."\">".($sn+1).".</a> ".$sections[$sn]."</b>\n";
    print "  <ul>\n";
    for $qn (0 .. $#{$questions[$sn]}) {
        $sq = ($sn+1).".".($qn+1);
        print "  <li> <a href=\"#question_".$sq."\">".$sq.".</a> ".$questions[$sn]->[$qn]."\n";
        #print "  <li> ".$sq.". ".$questions[$sn]->[$qn]."\n";
    }
    print "  </ul>\n";
}
print "</ul>\n";

# Emit main sections
for $sn (0 .. $#sections) {
    print "<hr>\n";
    print "<a name=section_".($sn+1).">\n";
    #print "<b><font size=+2 face=\"arial,helvetica\">".($sn+1).". ".$sections[$sn]."</font></b>\n";
    print "<?php pretty_h2(\"".($sn+1).". ".$sections[$sn]."\"); ?>\n";
    if ($sect_text[$sn] ne "") {
        print "<pre>\n";
        print $sect_text[$sn];
        print "</pre>\n";
    }
    for $qn (0 .. $#{$questions[$sn]}) {
        $sq = ($sn+1).".".($qn+1);
        print "<p>\n";
        print "<a name=question_".$sq.">\n";
        print "<font size=+1 face=\"arial,helvetica\">".$sq.". ".$questions[$sn]->[$qn]."</font>\n";
        print "<pre>\n";
        print $answers[$sn]->[$qn];
        print "</pre>\n";
    }
}

# Print footer
if ($#epilogue >= 0) {
    print @epilogue;
} else {
print <<EOF;
</body>
</html>
EOF
}


#{{{  sub guard {
sub guard {
# Hide wierd tags etc
    my ($x) = @_;
    return $x;
}
#}}}


