CREATE OR REPLACE FUNCTION fasttruncate(text)
RETURNS void AS 'MODULE_PATHNAME'
LANGUAGE C RETURNS NULL ON NULL INPUT VOLATILE;
