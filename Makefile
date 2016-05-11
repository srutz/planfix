
MODULE_big = planfixx
OBJS = planfix.o

#EXTENSION = test_parser
#DATA = test_parser--1.0.sql test_parser--unpackaged--1.0.sql

#REGRESS = test_parser

PG_CONFIG = /home/sr/opt/postgresql/bin/pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

include $(PGXS)


