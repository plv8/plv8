do $$
  Object.prototype[Symbol.iterator] = function() {
     return {
       next:() => this
     }
  };
  [...({})];
$$ language plv8;
