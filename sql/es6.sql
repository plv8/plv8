-- ES6 / harmony features
CREATE TABLE rectangle (
   id    INTEGER,
   data  JSON
);
INSERT INTO rectangle (id, data) VALUES
(1, '{"width": 20.3, "height": 1.5}'),
(2, '{"width": 10.2, "height": 9.5}'),
(3, '{"width": 3.5,  "height": 5.2}'),
(4, '{"width": 8.2,  "height": 8.2}'),
(5, '{"width": 9.4,  "height": 0.2}'),
(6, '{"width": 1.2,  "height": 1.5}');

-- for..of loop over result array with break
CREATE OR REPLACE FUNCTION get_rectangles_area(min_area NUMERIC)
   RETURNS json AS $$
      var rectangles = plv8.execute('SELECT id, data FROM rectangle ORDER BY id');
      var result = [];
      var area = 0.0;
      for (var rectangle of rectangles) {
	 area += rectangle.data.width * rectangle.data.height;
	 result.push({id: rectangle.id, data: rectangle.data});
	 if (area >= min_area) {
	    break;
	 }
      }
      return result;
   $$ LANGUAGE plv8 STABLE STRICT;
SELECT get_rectangles_area(130.0);

-- same for..of loop, this time using a cursor within a generator
CREATE OR REPLACE FUNCTION get_rectangles_area(min_area NUMERIC)
   RETURNS json AS $$
      var plan = plv8.prepare('SELECT id, data FROM rectangle ORDER BY id');
      var cursor = plan.cursor();
      var generator = function* () {
	 var row;
	 while (row = cursor.fetch()) {
	    yield row;
	 }
      };
      var rectangles = generator();
      var result = [];
      var area = 0.0;
      for (var rectangle of rectangles) {
	 area += rectangle.data.width * rectangle.data.height;
	 result.push({id: rectangle.id, data: rectangle.data});
	 if (area >= min_area) {
	    break;
	 }
      }
      cursor.close();
      plan.free();
      return result;
   $$ LANGUAGE plv8 STABLE STRICT;
SELECT get_rectangles_area(130.0);
