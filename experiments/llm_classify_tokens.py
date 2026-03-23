"""Ask the LLM to classify unresolved tokens into domain categories.

Extracts clean unresolved tokens from test documents, sends them to
Claude for classification, then identifies which new domains we should
add to the registry.
"""
import json
import duckdb
import subprocess
from pathlib import Path

BBOXES_EXT = "/Users/paulharrington/checkouts/blobboxes/build/duckdb/bboxes.duckdb_extension"
BLOBFILTERS_EXT = "/Users/paulharrington/checkouts/blobfilters/build/duckdb/blobfilters.duckdb_extension"
SYNTH = Path("/Users/paulharrington/checkouts/blobboxes/test_data/synthetic")

def get_clean_tokens(filepath: str, limit: int = 200) -> list:
    """Extract clean unresolved tokens from a document."""
    con = duckdb.connect(":memory:", config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{BBOXES_EXT}'")
    con.execute(f"LOAD '{BLOBFILTERS_EXT}'")
    con.execute("INSTALL postgres; LOAD postgres;")
    con.execute("ATTACH 'host=/tmp dbname=rule4_test' AS pg (TYPE POSTGRES)")

    con.execute("""
        CREATE TEMPORARY TABLE domain_filters AS
        SELECT domain_name, bf_from_base64(filter_b64) AS filter_bitmap
        FROM pg.domain.enumeration WHERE filter_b64 IS NOT NULL
    """)

    # Get existing domain names for exclusion
    existing = set(r[0] for r in con.execute(
        "SELECT domain_name FROM pg.domain.enumeration"
    ).fetchall())

    tokens = con.execute(f"""
        WITH WORDS AS (
            SELECT DISTINCT text
            FROM bb('{filepath}')
            WHERE LENGTH(text) BETWEEN 4 AND 30
              AND TRY_CAST(REPLACE(REPLACE(text, ',', ''), '.', '') AS BIGINT) IS NULL
              AND NOT regexp_matches(text, '^\d')
              AND NOT regexp_matches(text, '[,;:\[\]()\{{\}}]')
              AND text = TRIM(text)
              AND LENGTH(regexp_replace(text, '[^a-zA-Z ]', '', 'g')) > LENGTH(text) * 0.8
        ),
        -- Exclude tokens already matched by blobfilters
        PROBED AS (
            SELECT w.text,
                   MAX(bf_containment_json_normalized(w.text, df.filter_bitmap)) AS best_score
            FROM WORDS AS w
            CROSS JOIN domain_filters AS df
            GROUP BY w.text
        )
        SELECT text FROM PROBED
        WHERE best_score = 0
        ORDER BY text
        LIMIT {limit}
    """).fetchall()

    con.close()
    return [r[0] for r in tokens]


def main():
    # Gather tokens from multiple docs
    files = [
        str(SYNTH / "pmc_clinical.pdf"),
        str(SYNTH / "pmc_crips.pdf"),
        str(SYNTH / "05_countries_footnotes.pdf"),
    ]

    all_tokens = set()
    for f in files:
        if Path(f).exists():
            print(f"Extracting from {Path(f).name}...")
            tokens = get_clean_tokens(f, limit=150)
            print(f"  {len(tokens)} clean unresolved tokens")
            all_tokens.update(tokens)

    token_list = sorted(all_tokens)
    print(f"\nTotal unique clean tokens: {len(token_list)}")

    # Build the LLM prompt
    prompt = f"""I have {len(token_list)} text tokens extracted from scientific/medical PDF documents
that were NOT matched by any existing domain filter. These are tokens that appeared
in table cells and body text after filtering out numbers, dates, codes, and known
domain members.

Existing domains I already have (do NOT suggest these):
academic_disciplines, academic_degrees, admin_regions, airports, animals,
boolean_labels, building_types, canadian_provinces, chemical_elements, colors,
compass, companies_sp500, continents, countries, country_iso2, country_iso3,
country_names, crime_categories, currencies, days_long, days_short, diseases,
ethnic_groups, family_names, file_formats, food_items, gender, given_names,
government_agencies, http_methods, industries, land_use, legal_forms,
languages_major, materials, medical_specialties, medical_tests, medications,
mime_types_common, months_long, months_short, nationalities, occupations,
plants, programming_languages, quarters, religions, school_types, software,
sports, status_values, stock_exchanges, symptoms, units_of_measurement,
universities, us_census_race, us_cities_major, us_counties, us_state_abbrev,
us_states, utility_types, vehicle_makes, world_cities, world_countries

Here are the unresolved tokens (one per line):
{chr(10).join(token_list)}

For each token, classify it as ONE of:
- An existing domain name from the list above (if I missed it)
- A NEW domain category that would be useful to add (suggest a name)
- "common_english" if it's just a regular English word (not a proper noun or domain term)
- "fragment" if it's a broken/partial word
- "person_name" if it's a person's name (first or last)

Return your answer as a JSON object where:
- Keys are the suggested NEW domain names (not existing ones)
- Values are arrays of the tokens that belong to that domain
- Include a "common_english" key for regular words
- Include a "person_name" key for names
- Include a "fragment" key for broken words
- Include "already_covered" with sub-keys for existing domains I should have caught

Only suggest new domains that would have at least 3 members from these tokens
AND would be useful for classifying table column values in general documents.

Return ONLY the JSON, no explanation."""

    # Save prompt for manual review / LLM call
    prompt_path = SYNTH / "llm_classify_prompt.txt"
    with open(prompt_path, "w") as f:
        f.write(prompt)
    print(f"\nPrompt saved to {prompt_path}")
    print(f"Prompt length: {len(prompt)} chars, {len(token_list)} tokens to classify")

    # Show a sample
    print(f"\nSample tokens:")
    for t in token_list[:30]:
        print(f"  {t}")
    print(f"  ... ({len(token_list) - 30} more)")


if __name__ == "__main__":
    main()
