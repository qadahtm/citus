/* citus--8.4-1--9.0-1 */

SET search_path = 'pg_catalog';

DROP EXTENSION IF EXISTS shard_rebalancer;

-- get_rebalance_table_shards_plan shows the actual events that will be performed
-- if a rebalance operation will be performed with the same arguments, which allows users
-- to understand the impact of the change overall availability of the application and
-- network trafic.
--
CREATE OR REPLACE FUNCTION get_rebalance_table_shards_plan(relation regclass,
                                                           threshold float4 default 0.1,
                                                           max_shard_moves int default 1000000,
                                                           excluded_shard_list bigint[] default '{}')
    RETURNS TABLE (table_name regclass,
                   shardid bigint,
                   shard_size bigint,
                   sourcename text,
                   sourceport int,
                   targetname text,
                   targetport int)
    AS 'MODULE_PATHNAME'
    LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION get_rebalance_table_shards_plan(regclass, float4, int, bigint[])
IS 'returns the list of shard placement moves to be done on a rebalance operation';

-- get_rebalance_progress returns the list of shard placement move operations along with
-- their progressions for ongoing rebalance operations.
--
CREATE OR REPLACE FUNCTION get_rebalance_progress()
  RETURNS TABLE(sessionid integer,
                table_name regclass,
                shardid bigint,
                shard_size bigint,
                sourcename text,
                sourceport int,
                targetname text,
                targetport int,
                progress bigint)
  AS 'MODULE_PATHNAME'
  LANGUAGE C STRICT;
COMMENT ON FUNCTION get_rebalance_progress()
    IS 'provides progress information about the ongoing rebalance operations';


-- replicate_table_shards uses the shard rebalancer's C UDF functions to replicate
-- under-replicated shards of the given table.
--
CREATE FUNCTION replicate_table_shards(relation regclass,
                                       shard_replication_factor int default current_setting('citus.shard_replication_factor')::int,
                                       max_shard_copies int default 1000000,
                                       excluded_shard_list bigint[] default '{}',
                                       shard_transfer_mode citus.shard_transfer_mode default 'auto')
  RETURNS VOID
  AS 'MODULE_PATHNAME'
  LANGUAGE C STRICT;
COMMENT ON FUNCTION replicate_table_shards(regclass, int, int, bigint[], citus.shard_transfer_mode)
    IS 'replicates under replicated shards of the the given table';

-- rebalance_table_shards uses the shard rebalancer's C UDF functions to rebalance
-- shards of the given relation.
--
CREATE OR REPLACE FUNCTION rebalance_table_shards(relation regclass,
                                                  threshold float4 default 0,
                                                  max_shard_moves int default 1000000,
                                                  excluded_shard_list bigint[] default '{}',
                                                  shard_transfer_mode citus.shard_transfer_mode default 'auto')
  RETURNS VOID
  AS 'MODULE_PATHNAME'
  LANGUAGE C STRICT VOLATILE;
COMMENT ON FUNCTION rebalance_table_shards(regclass, float4, int, bigint[], citus.shard_transfer_mode)
    IS 'rebalence the shards of the given table across the worker nodes (including colocated shards of other tables)';

RESET search_path;
