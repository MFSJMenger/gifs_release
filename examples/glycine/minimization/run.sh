#!/bin/bash
set -euo pipefail
export GMX_MAXBACKUP=-1

readonly NAME=gly_minimize

[ ! -f gifs_config.ini ] && cp ../gifs_config.ini .

grompp -f minim.mdp -c ../glycine.gro -p ../glycine.top -n ../gly.ndx -o ${NAME}.tpr
mdrun -v -deffnm ${NAME}

echo
echo "Your minimized structure should now be in ${NAME}.gro"
