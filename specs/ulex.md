Command line tool `out/ulex` to generate lex program

- calling: `out/ulex input_file.txt output_file.ll -m <mode_flag> -t <target_tripple>`
- with a little help
- input file format:
  - each line is a regexp, auto assigning action_id (starting from 1)
