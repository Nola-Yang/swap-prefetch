# Guiding Principles for Project Deku's Investigation into Prefetching

1.  **Core Problem Formulation: Binary Classification for Prefetching**
    *   **Principle:** Every potential prefetch operation will be decided through a binary classification lens: "Given a target memory address/page, should it be prefetched (Yes) or not (No)?"
    *   **Scope:** The primary granularity for prefetching decisions will be at the memory page level.

2.  **Central Hypothesis: Pointer-Driven Prediction**
    *   **Principle:** Investigate the core hypothesis that pointers within a currently accessed memory page, particularly those located towards the end of the page, serve as strong indicators for predicting the next memory page to be accessed.
    *   **Initial Validation:** Begin with controlled benchmark applications where pointer locations are deliberately known and consistent (e.g., fixed at page ends). This will validate the fundamental concept and facilitate initial data collection for model training.

3.  **Candidate Address Generation and Access Pattern Analysis**
    *   **Principle:** Employ existing aggressive prefetching algorithms or techniques primarily as a mechanism to generate a viable and comprehensive set of *candidate addresses* for prefetching.
    *   **Analysis:** Evaluate application memory access patterns by observing which of these candidate addresses are actually accessed, providing insights into the potential and limitations of aggressive approaches.

4.  **Modeling Strategy: Evolving Decision Trees**
    *   **Principle:** Develop, train, and refine decision tree models to perform the binary classification task (i.e., to decide whether to prefetch a candidate page or not).
    *   **Evolutionary Approach:**
        *   *Phase 1 (Benchmark-driven):* Initial models will be trained using data from benchmarks where pointer existence and location are explicit and controlled.
        *   *Phase 2 (Real-system Adaptation):* Subsequent models will aim to dynamically identify and predict pointer locations within general page content by analyzing page data and memory access patterns. Features for the decision tree will expand to include characteristics of page content, access history, and inferred pointer likelihoods.

5.  **Data Collection and Feature Engineering for Model Training**
    *   **Principle:** Systematically collect comprehensive data, including:
        *   Addresses of currently accessed pages.
        *   Candidate prefetch addresses (generated as per Principle 3).
        *   Ground truth: Actual next accessed pages.
        *   Features related to current pages: presence/absence of pointers, pointer values, location of pointers (initially fixed, later predicted).
        *   Page content characteristics and memory access history.
    *   **Current Focus:** The ongoing work on "pointer analysis for swap collecting data" is a direct implementation of this principle, crucial for feature engineering and training effective decision trees.

6.  **Iterative Research and Development Path**
    *   **Principle:** Follow a structured, iterative research path:
        *   **Foundation:** Start with simplified, controlled benchmarks to validate core concepts and gather clean initial data.
        *   **Progression:** Gradually transition to more complex and realistic scenarios, culminating in a system where decision trees dynamically identify pointers and make prefetching decisions in general applications.

7.  **Deployment and Rigorous Performance Evaluation**
    *   **Principle:** Deploy the trained decision tree models as a runtime prefetching component within the "ExtMem" system.
    *   **Evaluation Criteria:** Measure the success of Deku by:
        *   Prefetch accuracy (correctly prefetched pages vs. unnecessary prefetches).
        *   Impact on overall system performance (e.g., reduced memory access latencies, improved application execution times).
        *   Comparison against baseline scenarios (e.g., no prefetching, standard aggressive prefetchers). 