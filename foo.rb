module Querying
  def find_by_sql(query)
    puts "Querying#find_by_sql(#{query.inspect})"
  end
end

module Instrumentation
  def find_by_sql(query)
    puts "Instrumentation#find_by_sql(#{query.inspect})"
    super
  end
end

class Post
  extend Querying
end

# If this is moved _above_ where `Post` is defined, Instrumentation is invoked.
Querying.class_eval do
  prepend Instrumentation
end

puts Post.method(:find_by_sql).source_location.inspect
Post.find_by_sql("SELECT * FROM posts")
