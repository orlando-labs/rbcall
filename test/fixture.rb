module SputnikBridgeFixture
  class << self
    attr_reader :ensures, :continued
    def around
      @continued = false
      @ensures = 0
      begin
        result = yield 7
        @continued = true
        result
      ensure
        @ensures += 1
      end
    end
    def save(&block)
      @saved = block
      nil
    end
    def run(value)
      @saved.call(value)
    end
    def keywords(value:, factor:)
      value * factor
    end
  end
end
