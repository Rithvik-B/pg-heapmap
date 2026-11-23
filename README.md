PostgreSQL Database Management System
=====================================

> **CS 631 project: the heapmap index.** This tree is PostgreSQL 18.7 plus one
> addition, `contrib/heapmap`: a learned block-range index access method
> (`CREATE INDEX ... USING heapmap`) for columns that follow the table's
> physical order. Start at [contrib/heapmap/README.md](contrib/heapmap/README.md).
> The first commit of this repository is the unmodified PostgreSQL source;
> everything else is in the commits after it.

This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions.  This distribution also contains C language bindings.

Copyright and license information can be found in the file COPYRIGHT.

General documentation about this version of PostgreSQL can be found at
<https://www.postgresql.org/docs/18/>.  In particular, information
about building PostgreSQL from the source code can be found at
<https://www.postgresql.org/docs/18/installation.html>.

The latest version of this software, and related software, may be
obtained at <https://www.postgresql.org/download/>.  For more information
look at our web site located at <https://www.postgresql.org/>.
