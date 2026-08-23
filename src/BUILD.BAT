@ECHO OFF
REM Run non-interactively by dosemu2 -E, from the host-side Makefile in
REM the repo root. Kept as a real .BAT rather than chained on the -E
REM command line itself: DOS's COMMAND.COM has no && operator, so a
REM one-line "D: && CD ... && MAKE ..." from the host doesn't chain the
REM way it would in bash -- each step needs its own line, which is
REM exactly what a .BAT file is for.
REM
REM We copied ./legacy to the dosemu2 C: drive in the host-side Makefile,
REM so this script can just assume that the Borland C++ 3.1 install is
REM available at C:\BC31. This will set the include directory and library, 
REM just as would need to be set in the autoexec.bat if you were running 
REM Borland C++ 3.1 interactively in a DOS box.
REM 
REM C:\TEMP is made by make, cpp and the Borland tools, but it's included 
REM as an empty directory in the repo so that just incase that breaks it
REM will be there for the toolset to use.

@ECHO OFF
PROMPT $p$g

SET INCLUDE=C:\BC31\INCLUDE\
SET LIB=C:\BC31\LIB
SET TEMP=C:\TEMP

PATH C:\;C:\BC31\BIN;%PATH%

MAKE -f MAKEFILE %1 %2 %3
