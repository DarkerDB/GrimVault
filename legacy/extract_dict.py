import yaml

# Read the YAML config file
with open('models/paddleocr/latin_PP-OCRv5_mobile_rec_infer/inference.yml', 'r', encoding='utf-8') as f:
  config = yaml.safe_load(f)

  # Extract the character dictionary
  chars = config['PostProcess']['character_dict']

  # Write to new dictionary file
with open('models/paddleocr/latin_pp_ocrv5_dict.txt', 'w', encoding='utf-8') as f:
  for char in chars:
    f.write(f"{char}\n")

print(f"Extracted {len(chars)} characters")
print(f"Saved to: models/paddleocr/latin_pp_ocrv5_dict.txt")
