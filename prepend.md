# Module\#prepend

When a `Module` is `include`d into another `Module` (or `Class`, a more specific kind of `Module`), it is added to the `ancestor` chain _after_ the `Module` where it was included. For example:

```ruby
module B
end

class A
  include B
end

A.ancestors # => [A, B, ...]
```

With `Module#prepend`, though, the prepended `Module` is added _before_ the `Module` where it was prepended. For example:

```ruby
module B
end

class A
  prepend B
end

A.ancestors # => [B, A, ...]
```

This allows methods in B to override methods in A without `alias_method`.

The situation is more complicated when `Module#prepend` interacts with second `Module` that is then `include`d or `extend`ed onto a third `Module` (or `Class`).

```ruby
module C
end

module B
  prepend C
end

class A
  include B
end
  
A.ancestors # => [A, C, B, ...]
```

I think what most folks would expect to happen. B is `include`d into A, and C has been prepended to B.

But what happens if `C` is `prepend`ed _after_ `B` has been `include`d into A?

```ruby
module C
end

module B
end

class A
  include B
end

B.class_eval do
  prepend C
end

A.ancestors # => [A, B, ...]
```

The ancestor chain for A is not modified when C is later prepended to B.

Until we figure out a way around this load-order dependency, I suggest avoiding using `prepend` on `Module`s that are included or extended onto other `Module`s. Prepending to `Module`s (or `Class`es) like `ActiveRecord::Base` (that is directly in the ancestor chain of ActiveRecord models) is OK, but `prepend`ing to `ActiveRecord::Querying` that is itself `include`d into `ActiveRecord::Base` will be load-order dependent and is worth avoiding at this time.
