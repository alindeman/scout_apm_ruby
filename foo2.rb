module AbstractAdapter
  def log(query)
    puts "AbstractAdapter#log(#{query.inspect})"
  end
end

module Instrumentation
  def log(query)
    puts "Instrumentation#find_by_sql(#{query.inspect})"
    super
  end
end

class MySQLAdapter < AbstractAdapter
  include 
end

# If this is moved _above_ where `Post` is defined, Instrumentation is invoked.
Querying.class_eval do
  prepend Instrumentation
end

puts Post.method(:find_by_sql).source_location.inspect
Post.find_by_sql("SELECT * FROM posts")
