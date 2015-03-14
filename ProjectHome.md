plv8js is a procedural language add-on for PostgreSQL, which means you can define JavaScript functions that run inside a PostgreSQL server using google V8 Engine.

Go to http://pgxn.org/dist/plv8/ to download the source tarball.
Since google doesn't allow to create more [download](http://google-opensource.blogspot.com/2013/05/a-change-to-google-code-download-service.html)

### Quick View ###

Here is an example.

```
CREATE FUNCTION to_jsontext(keys text[], vals text[]) RETURNS text AS
$$
	var o = {};
	for (var i = 0; i < keys.length; i++)
		o[keys[i]] = vals[i];
	return JSON.stringify(o);
$$
LANGUAGE plv8 IMMUTABLE STRICT;

SELECT to_jsontext(ARRAY['age', 'sex'], ARRAY['21', 'female']);
         to_jsontext         
-----------------------------
 {"age":"21","sex":"female"}
(1 row)
```