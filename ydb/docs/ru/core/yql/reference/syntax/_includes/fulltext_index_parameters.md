  * общие параметры для всех полнотекстовых индексов:
    * `tokenizer` - тип токенизатора (`standard` или `whitespace`)
    * `use_filter_lowercase` - фильтр приведения к нижнему регистру (`true` или `false`)
    * `use_filter_snowball` - фильтр стемминга [Snowball](https://snowballstem.org/) (`true` или `false`)
    * `language` - язык для стеммера [Snowball](https://snowballstem.org/) (например, `english`, `russian`)
    * `use_filter_ngram` - фильтр [N-грамм](https://en.wikipedia.org/wiki/N-gram) (`true` или `false`)
    * `use_filter_edge_ngram` - фильтр краевых [N-грамм](https://en.wikipedia.org/wiki/N-gram) (`true` или `false`)
    * `filter_ngram_min_length` - минимальная длина N-граммы (положительное целое)
    * `filter_ngram_max_length` - максимальная длина N-граммы (положительное целое, \(\ge\) `filter_ngram_min_length`)
