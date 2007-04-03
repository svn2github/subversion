require "svn/util"
require "svn/ext/core"

module Svn
  class Error < StandardError
    class << self
      # This is for backward compatibility with 1.4 or earlier.
      def new_corresponding_error(*args)
        SvnError.new_corresponding_error(*args)
      end
    end

    class SvnError < Error
      TABLE = {}

      class << self
        def new_corresponding_error(code, message, file=nil, line=nil)
          if TABLE.has_key?(code)
            TABLE[code].new(message, file, line)
          else
            new(code, message, file, line)
          end
        end
      end

      attr_reader :code, :error_message, :file, :line
      def initialize(code, message, file=nil, line=nil)
        @code = code
        @error_message = message
        @file = file
        @line = line
        msg = ""
        if file
          msg << "#{file}"
          msg << ":#{line}" if line
          msg << " "
        end
        msg << @error_message
        super(Converter.to_locale_encoding(msg))
      end
    end

    Ext::Core.constants.each do |const_name|
      if /^SVN_ERR_(.*)/ =~ const_name
        error_const_name = $1
        next if /_CATEGORY_START\z/ =~ error_const_name
        error_class_name = Util.to_ruby_class_name(error_const_name)
        value = Ext::Core.const_get(const_name)
        module_eval(<<-EOC, __FILE__, __LINE__ + 1)
          class #{error_class_name} < SvnError
            def initialize(message="", file=nil, line=nil)
              super(#{value}, message, file, line)
            end
          end
          # This is for backward compatibility with 1.4 or earlier.
          #{error_const_name} = #{error_class_name}
        EOC
        SvnError::TABLE[value] = const_get(error_class_name)
      end
    end

    class FsAlreadyClose < Error
    end

    class ReposAlreadyClose < Error
    end

    WcMismatchedChangeList = WcMismatchedChangelist
  end
end
