Planfix is a PostgresQL Extension to force specific indices..

Planfix is an extension that allows the user to explicitly force the 
use of a specific index in PostgresQL.

THe particular usecase were Querries that could have (and should have)
used a full-text-index (GIN) but the planner thought it would be a 
better choice to use another index. Reason was (afaics) the multitude
of columns querried and thus the supposed high selectively combined 
with a limit clause, leading to use another index.

To install:

Copy into your postgresql source-tree contrib directory or compile
from anywhere. Check the makefile for the path to pgxs


Usage:

Set a table and a number of indices to force like this:

set planfix.forcedindex = 'mytable,myindex1,myindex2'

You can set mulitple Tables and indices by appending clauses like the
one above and separting them with with ;

After your critial query you can reset the behavior by doing

set planfix.forcedindex = ''




Written by stepan.rutz@gmx.de

For more information check the sourcecode and inline comments of
planfix.c





