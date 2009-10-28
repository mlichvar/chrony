#!/usr/bin/perl
#                Copyright (C) Paul Elliott 2002
my($copyrighttext) =  <<'EOF';
#                Copyright (C) Paul Elliott 2002
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation; either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the Free Software
#   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#   SEE COPYING FOR DETAILS
EOF

#modules we use.

use Socket;
use Getopt::Std;
use Net::DNS;
use Tie::Syslog;
use File::Temp qw/ :mktemp  /;
use File::Copy;

local($res) = new Net::DNS::Resolver;

#dns lookup of IP address.
#returns ip or errorstring.
sub gethostaddr($)                                 #get ip address from host
{
  my($host) = shift;
  $query = $res->search($host);
  if ($query) {
    foreach $rr ($query->answer) {
      next unless $rr->type eq "A";
      print $rr->address, "\n" if  $pedebug;
      return $rr->address;
    }
  } else {
    print "query failed: ", $res->errorstring, "\n" if $pedebug;
    return $res->errorstring;
  }	

}

#send messages to syslog

sub Log($$)
  {
    if ($log) {
      my($level) = shift;
      my($mess) =shift;

      tie *MYLOG, 'Tie::Syslog',$level,$0,'pid','unix';
      print MYLOG $mess;

      untie *MYLOG;
    }
  }

#send message to output or syslog
#and die.

sub BadDie($)
{
  my($myerr) =$!;
  my($mess)=shift;

  if($log){
      tie *MYLOG, 'Tie::Syslog','local0.err',$0,'pid','unix';
      print MYLOG $mess;
      print MYLOG $myerr;

      untie *MYLOG;

  } else {
    print "$mess\n$myerr\n";
  }
  die $mess;
}

sub isIpAddr($)                     #return true if looks like ip address
{
  my($ip) = shift;
  return 1 if ( $ip =~ m/$ipOnlyPAT/ );
  return 0;
}
sub isHostname($)                     #return true if looks like ip address
{
  my($ip) = shift;
  return 1 if ( $ip =~ m/$hostnameOnlyPAT/ );
  return 0;
}

#send commands to chronyc by piping.
sub chronyc($)                              #send commands to chronyc
{
  my($command) = shift;
  my($err) = "/var/tmp/chronyc.log";
  my($chronyP) = "/usr/local/bin/chronyc";
  open(CHRONY, "| $chronyP 1>$err 2>&1");

  print CHRONY "$passwd$command\n";

  close(CHRONY);

  Log('local0.info',"chronyc command issued=$command");
                                   #look at status lines till return bad.
  open( IN, "<$err");
  my($status);
  while (<IN>) {
  $status = $_;

    unless ( m/\A200 OK/ ) {
      last;
    }

  }

  $status ="" if ( $status =~ m/\A200 OK/ );
  close(IN);
  unlink $err;
  Log('local0.info',"chronyc results=$status");
  return $status;

}

#common patterns

# an ip address patern
local($ipPAT) = qr/\d{1,3}(?:\.\d{1,3}){3}/;
# an hostname pattern
local($hostnamePAT) = qr/\w+(?:\.\w+)*/;
#line with hostname only
local($hostnameOnlyPAT) = qr/\A$hostnamePAT\Z/;
#line with ip address only
local($ipOnlyPAT) =qr/\A$ipPAT\Z/;

#options hash
my(%opts);


getopts('nuadslPSC', \%opts);

local($log) = ( $opts{'l'} ) ? 1 : 0;

my($offline) = !( $opts{'n'} ) ;
my($offlineS) = ( $opts{'n'} ) ? " " : " offline" ;

# paul elliotts secret debug var. no one will ever find out about it.
local($pedebug)=( ($ENV{"PAULELLIOTTDEBUG"}) or ($opts{P}) );

if ($opts{C}) {

  print $copyrighttext;
  exit 0;
}


print <<"EOF" unless $opts{'S'};
$0, Copyright (C) 2002 Paul Elliott
$0 comes with ABSOLUTELY NO WARRANTY; for details
invoke $0 -C.  This is free software, and you are welcome
to redistribute it under certain conditions; invoke $0 -C
for details.
EOF



local($passwd);

# password to send to chronyc
my($pl) = $ENV{"CHRONYPASSWORD"};

#password comand to send to chronyc
if ( $pl ) {
  $passwd = "password $pl\n";
} else {
  $passwd = "";
}
print "passwd=$passwd\n" if ($pedebug);

my(%host2ip);

# hash of arrays. host2ip{$host}[0] is ip address for this host
# host2ip{$host}[1]   is rest of paramenters for this host exc offline.

#if debuging do chrony.conf in current directory.
my($listfile) =( ($pedebug) ? "./chrony.conf" : "/etc/chrony.conf") ;

# This section reads in the old data about
# hostnames IP addresses and server parameters
# data is stored as it would be in chrony.conf
# file i.e.:
#># HOSTNAME
#>server IPADDR minpoll 5 maxpoll 10 maxdelay 0.4 offline 
#
# the parameter offline is omitted if the -n switch is specified.
# first parameter is the filename of the file usually
# is /etc/DNSchrony.conf
# this is where we store the list of DNS hosts.
# hosts with static IP address shold be kept in chrony.conf

# this is header that marks dnyamic host section
my($noedithead)=<<'EOF';
## DNSchrony dynamic dns server section. DO NOT EDIT
## per entry FORMAT:
##        |--------------------------------------------|
##        |#HOSTNAME                                   |
##        |server IP-ADDRESS extra-params [ offline ]  |
##        |--------------------------------------------|
EOF
#patern that recognizes above.
my($noeditheadPAT) = 
qr/\#\#\s+DNSchrony\s+dynamic\s+dns\s+server\s+section\.\s+DO\s+NOT\s+EDIT\s*/;

#end of header marker.
my($noeditheadend)=<<'EOF';
## END OF DNSchrony dynamic dns server section.
EOF

#pattern that matches above.
my($noeditheadendPAT)=
qr/\#\#\s+END\s+OF\s+DNSchrony\s+dynamic\s+dns\s+server\s+section.\s*/;

#array to hold non dns portion of chrony.conf
my(@chronyDconf);


my($ip);
my($rest);
my($host);

# for each entry in the list of hosts....
open(READIN, "<$listfile") or  BadDie("Can not open $listfile");

# read till dynamic patern read save in @chronyDconf

while ( <READIN> ) {
  
  my($line) = $_;

  last if ( m/\A$noeditheadPAT\Z/ );

  push(@chronyDconf,$line);

}

while ( <READIN> ) {
 
   #end loop when end of header encountered
   last if ( m/\A$noeditheadendPAT/ );

   # parse the line giving ip address, extra pamamters, and host
   #do host comment line first
   ($host) = m{
	       \A\#\s*
	       ($hostnamePAT)
               \s*\z
              }xio;

  #no match skip this line.
  next unless (  $host );

  # read next line
  $_ = <READIN>;

  # parse out ip address extra parameters.
  ($ip,$rest) =
  m{
    \A
    \s*
    server                                       #server comand
    \s+
    ($ipPAT)                                     #ip address
    (?ixo: \s )
    \s*
    (
    (?(?!
            (?iox: offline )?                    #skip to offline #
            \s*                                  #or #
            \Z
    ).)*
    )
    (?ixo:
     \s*
     (?ixo: offline )?                          #consume to #
     \s*
     \Z
    )
    }xio ;

  #if failure again.
  next unless (  $ip );

  $rest =~ s/\s*\z//;                          #remove trail blanks
                                               #from parameters
  # store the data in the list
  # key is host name value is
  # array [0] is ip address
  #       [1] is other parameters
  $host2ip{$host} = [$ip,$rest] ;
  print "ip=$ip rest=$rest host=$host<\n" if $pedebug;

}
#read trailing line into @chronyDconf
while ( <READIN> ) {

  push(@chronyDconf,$_);

}

close(READIN) or BadDie("can not close $listfile");

#if the add command:
# command can be HOST=IPADDRESS OTHER_PARAMETERS
# means add the server trust the ip address geven with out a dns lookup
# good for when dns is down but we know the ip addres
# or
#    HOST OTHER_PARAMETERS
#we lookup the ip address with dns.

if ($opts{'a'}) {
  my($param)= shift;


  # parse the param is it hostname
  if ( ($host,$ip) = $param =~ m/\A($hostnamePAT)=($ipPAT)\Z/ ) {
    printf "ip=$ip host=$host\n" if ($pedebug);
  } else {

    $host = $param;

    # get the ip address
    $ip = gethostaddr($host);

    if ( ! isIpAddr($ip) or ! isHostname($host) ) {
      print "query failed: ", $ip, "host=$host\n" if $pedebug;
      exit 1;
    } 
  }
  printf "ip=$ip host=$host\n" if ($pedebug);

  # add the server using chronyc
  my($status) = chronyc("add server $ip $rest");
  if ($status) {		#chronyc error
    print "chronyc failed, status=$status\n";
    exit 1;
  }

  # get rest of arguements
  $rest = join( ' ', @ARGV);
  print "rest=$rest\n" if ($pedebug);

  #save node in hash
  $host2ip{$host} = [$ip,$rest] ;
  print "ip=$ip rest=$rest host=$host<\n" if $pedebug;

}

#delete command if arguement is ip address
#just delete it
#if a hostname look it up
#then delete it.

if ($opts{'d'}) {
  $host = shift;

  #get host name is it ap address
  if ( isIpAddr($host) ) {                           # if ip address
    my($hostIT);
    my($found) =0;
    foreach $hostIT (keys(%host2ip) ) {               #search for match
      if ( $host2ip{$hostIT}[0] eq $host) {
	$found=1;                                     #record match
      }
    }                                                 #end of search
    if ($found) {                                     #if match found
      my($status) = chronyc("delete $host");          #chronyc
      if ($status) {                                  #chronyc error
	print "chronyc failed, status=$status\n";
	exit 1;
      } else {                                        #reiterate
	foreach $hostIT (keys(%host2ip) ) {
	  if ( $host2ip{$hostIT}[0] eq $host) {
	    delete $host2ip{$hostIT};                 #deleting match hosts
	  }
	}

      }

    }
  } else {                                          #else not ip address
                                                    #must be hostname
    if ( ! $host2ip{$host} ) {
      print "No such host as $host listed\n";
      exit 1;
    }
                                                    #get ip address
    $ip=gethostaddr($host);
    if ( ! isIpAddr($ip) ) {                        #no ip address
      print "query failed: ", $ip, "\n" if $pedebug;
      exit 1;
    } 

    printf "ip=$ip host=$host\n" if ($pedebug);

    my($listed_host_ip) = $host2ip{$host}[0];       # get the ip address saved

    if ( $ip ne $listed_host_ip) {
      print 
	"Info: listed host ip=>$listed_host_ip".
        "< is different from DNS ip=>$ip<\n";
      $ip = $listed_host_ip;
    }

    # delete the server
    my($status) = chronyc("delete $listed_host_ip\n");

    if ($status) {
      print "chronyc failed, status=$status\n";
      exit 1;
    }
    #delete table entry
    delete$host2ip{$host};
  }

}

#update for each host who's dns ip address has changed
#delete the old server and add the new. update the record.
if ($opts{'u'}) {
  my($command);

  my(%prospective);                        # store new IP address we
                                           #are thinking of changing.

  Log('local0.info',
      "Now searching for modified DNS entries.");

  foreach $host (keys(%host2ip)) {         #for each listed host
    my($old_ip) = $host2ip{$host}[0];      #get old ip
    $rest       = $host2ip{$host}[1];      #extra params

    $ip         = gethostaddr($host);      #get new ip from dns
                                           #if error
    if ( ! isIpAddr($ip) or ! isHostname($host) ) {
      print "query failed: ", $ip, "host=$host\n";

      Log('local0.err',"query failed: ". $ip . "host=$host");
      
      exit 1;
    } 

    next if($ip eq $old_ip);                #if ip not changed, skip

    Log('local0.info',"Ip address for $host has changed. Old IP address=".
                      "$old_ip, new IP address=$ip");
    # add command to delete old host, add the new.
    $command = $command . "delete $old_ip\n" .
               "add server $ip $rest\n";

    # we are now thinking about changing this host ip
    $prospective{$host} = [$ip,$rest];
  }
  # submit all the accumulated chronyc commands if any.
  if ($command) {
    $status = chronyc($command);
    if ($status) {
      print "chronyc failed, status=$status\n";
      Log('local0.err',"query failed: ". $ip . "host=$host");
      exit 1;
    }
  } else {                                  #if no commands exit
    exit 0;                                 #because no rewrite of file needed
  }

  #copy prospective modifications back into main table.
  #we now know that all these mods were done with chronyc
  foreach $host (keys(%prospective)) {
    my($ip) = $prospective{$host}[0];
    $rest       = $prospective{$host}[1];
    $host2ip{$host} = [$ip,$rest];
  }
}

#starting for each entry we have read in from the old list
# add the server in chronyc
# this option is seldom used.

if ($opts{'s'}) {
  my($command)="";

  foreach $host (keys(%host2ip)) {
    $command = $command . "add server $host2ip{$host}[0] ".
                          "$host2ip{$host}[1]\n";
  }
  my($status) = chronyc($command);
  if ($status) {
    print "chronyc failed, status=$status\n";
    exit 1;
  }

}
# write out the data file in format
#># HOSTNAME
#>server IPADDRESS extra parameters [offline] 
# offline is omitted if -n switch is specified.

my(@value);
my($such);
{
  # to start out we write to temporary file.
  (my($writeout) , my($outname)) = mkstemp( "${listfile}.outXXXXXXX");

  $outname or BadDie("can not open for $listfile");


  # save the chrony.conf part!
  # and write the DYNAMIC header
  print $writeout @chronyDconf, $noedithead;


  # for each entry
  foreach $host (keys(%host2ip) ){

    #write the record

    # write the comment that indicates the hostname
    # and the server command.
    print $writeout 
     "\# $host\nserver $host2ip{$host}[0] $host2ip{$host}[1]${offlineS}\n" ;

    print 
     "server $host2ip{$host}[0] $host2ip{$host}[1]${offlineS}\# $host\n" 
     if $pedebug;

  }

  #WRITE THE end of dnyamic marker comment
  print $writeout $noeditheadend;

  # close the output file which was a temporary file.
  close($writeout) or BadDie("can not close $outname");

  # we now begin a intracate dance to make the the temporary
  # the main chrony.conf
  #
  # if there is a chrony.conf.BAK save it to a temporary.
  # rename chrony.conf to chrony.conf.BAK
  # rename the temporary to chrony.conf
  # if there already was a chrony.conf.BAK, unlink the copy of this.

  my($backname) = "$listfile\.BAK";
  my($backplain)  = ( -f $backname );
  my($saveback);
  #if chrony.conf.BAK exists rename to a temporary.
  if ($backplain ) {

    $saveback = mktemp("${backname}.bakXXXXXXX");
    move($backname,$saveback) or 
         BadDie "unable to move $backname to $savename";

  }

  # rename old chrony.conf to chrony.conf.BAK
  move($listfile,$backname) or
       BadDie "unable to move $listfile to $backname";

  # rename our output to chrony.conf
  move($outname,$listfile) or
       BadDie "unable to move $outname to $listfile";

  #if there was a temporary chrony.conf.BAK that we saved to temp
  #unlink it
  unlink($saveback) or BadDie "unable to unlink $saveback" if($backplain);
  
}
