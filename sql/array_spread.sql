do $$
  Object.prototype[Symbol.iterator] = function() {
     return {
       next:() => this
     }
  };
  [...({})];
$$ language plv8;
do $$
  Object.prototype[Symbol.iterator] = function() {
     return {
       next:() => this
     }
  };
  [...({})];
$$ language plv8;
