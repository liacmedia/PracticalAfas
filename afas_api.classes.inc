<?php
/**
 * @file
 * Contains AfasSoapConnection and AfasSoapClient.
 */

/**
 * Class AfasSoapConnection.
 *
 * This contains general functions to make most AFAS SOAP calls and error
 * handling easier.
 */
class AfasSoapConnection {

  /**
   * Wrapper class around the SOAP client library.
   *
   * @var string
   */
  private $clientClassName;

  /**
   * Options for the SOAP client / call arguments. (These are merged into one
   * array, because they actually overlap - some are used for both.)
   *
   * @var array
   */
  private $options;

  /**
   * Ways of reporting errors.
   *
   * @var int
   */
  private $errorReporting;

  /**
   * Info about the last call.
   *
   * @var array
   */
  private $lastCallInfo;

  /**
   * Constructor function.
   *
   * @param string $client_class_name
   *   The name of the class to use which contains soap-client specific
   *   functionality in a callSoapFunction() method. It is almost never
   *   necessary to provide this argument, because 'default' uses the SoapClient
   *   class which comes bundled with PHP5.
   */
  public function __construct($client_class_name = 'default') {
    // By default, do not print errors to the screen.
    $this->errorReporting = AFAS_LOG_WATCHDOG;

    if (empty($client_class_name) || $client_class_name == 'default') {
      // It is possible to override usage of the standard client by another
      // class (e.g. 'AfasNusoapClient'), in a site-config variable. This is
      // supposedly used so infrequently that there is no config UI for this.
      $this->clientClassName = variable_get('afas_api_client_class', 'AfasNusoapClient');
      // @todo change this default to AfasSoapClient!
    }
    else {
      $this->clientClassName = $client_class_name;
    }
  }

  /**
   * Retrieve data from AFAS through a GET connector.
   *
   * @param string|int $data_id
   *   Identifier for the data. (Usually the name of the AFAS 'get connector',
   *   but this can differ with $data_type.)
   * @param array $filters
   *   Filters to apply before returning data
   * @param string $data_type
   *   Type of data to retrieve and, for get connectors, the format in which to
   *   return it.
   *   'get':        $data_id is a get connector; return data as array.
   *   'get_simplexml: $data_id is a get connector; return data as
   *                   SimpleXMLElement. This is slightly faster than 'get'
   *   'report':     $data_id is the report ID for a report connector.
   *   'attachment': $data_id is the 'subject ID' (int) for a subject connector.
   *   'data':       $data_id is the function name for which to retrieve the XSD
   *                 schema. (This used to be called "DataConnector"; in 2014
   *                 that term has disappeared from the online documentation.)
   * @param array $extra_arguments
   *   Other arguments to pass to the soap call, besides the ones which are
   *   hardcoded in this function for convenience. (For get connectors, these
   *   are 'connectorId' and 'filters'; for others, see code below.)
   *   Specifying this argument usually unnecessary. To see what it can be used
   *   for, check the code in normalizeArguments() and/or the WSDL/documentation
   *   of the AFAS SOAP endpoint.
   *
   * @return string|array|bool|SimpleXMLElement
   *   See $data_type; FALSE when error occurred.
   */
  function getData($data_id, $filters = array(), $data_type = 'get', $extra_arguments = array()) {

    if (!is_array($extra_arguments)) {
      return FALSE;
    }

    $function = '';
    if (is_string($data_type)) {
      switch (strtolower($data_type)) {
        case 'get':
        case 'get_simplexml':
          $extra_arguments['connectorId'] = $data_id;
          if (!empty($filters)) {
            $extra_arguments['filters'] = $filters;
          }
          $connector_type = 'get';
          $function = 'GetDataWithOptions';
          break;

        case 'report':
          $extra_arguments['reportID'] = $data_id;
          if (!empty($filters)) {
            $extra_arguments['filters'] = $filters;
          }
          $connector_type = 'report';
          $function = 'Execute';
          break;

        case 'attachment':
          $extra_arguments['subjectID'] = $data_id;
          $connector_type = 'subject';
          $function = 'GetAttachment';
          break;

        case 'data':
          // Oct 2014: I finally saw the first example of a 'DataConnector' in the
          // latest version of the online documentation, at
          // http://profitdownload.afas.nl/download/Connector-XML/DataConnector_SOAP.xml
          // (on: Connectors > Call a Connector > SOAP call > UpdateConnector,
          //  which is https://static-kb.afas.nl/datafiles/help/2_9_5/SE/EN/index.htm#App_Cnnctr_Call_SOAP_Update.htm)
          // Funny thing is: there is NO reference of "DataConnector" in the
          // documentation anymore!
          // dataID is apparently hardcoded (as far as we know there is no other
          // function for the so-called 'DataConnector' that getting XML schema):
          $extra_arguments['dataID'] = 'GetXmlSchema';
          $extra_arguments['parametersXml'] = "<DataConnector><UpdateConnectorId>$data_id</UpdateConnectorId><EncodeBase64>false</EncodeBase64></DataConnector>";
          $connector_type = 'data';
          $function = 'Execute';
      }
    }
    else {
      $this->handleError(array(
        'error_source' => 'call',
        'error' => t('AFASSoapConnection::getData() called with unknown data_type argument %type!',
          array('%type' => is_scalar($data_type) ? strval($data_type) : '<non-scalar>')),
        'error_message_safe' => TRUE,
      ));
    }

    if ($function) {
      $data = $this->callSoapFunction($function, $extra_arguments, $connector_type);
      if (isset($data[$function . 'Result'])) {

        // Check the (normalized) options which influence the output format.
        // See normalizeArguments() for default option values which are set in
        // $extra_arguments.
        if ($function == 'GetDataWithOptions') {
          $extra_arguments = $this->getlastCallInfo('arguments');
          if ($extra_arguments['options_array']['Outputmode'] == 2) {
            // We don't support text output. There seems to be no reason for it,
            // but if you see one, feel free to create/test/send in a patch.
            // (Possibly implementing $data_type = 'get_text' and overwriting
            // Outputmode above?)
            //
            // We've made the SOAP call for nothing. That's just to keep this
            // check inside this function, while still allowing subclasses to
            // override Outputmode.
            $this->handleError(array(
              'error_source' => 'call',
              'error' => 'The afas_api_get_data() function cannot handle text output!'
            ));
            return FALSE;
          }
        }

        // What to return?
        switch (strtolower($data_type)) {
          case 'get':
            // Walk through the SimpleXMLElement to create array of arrays (items)
            // of string values (fields). We assume each first-level XML element
            // is a row containing fields without any further nested tags.
            $doc_element = new SimpleXMLElement($data[$function . 'Result']);
            $items = array();

            if ($function == 'GetDataWithOptions' &&
                $extra_arguments['options_array']['Outputoptions'] == 3) {

              foreach ($doc_element as $row_element) {
                // The XML may contain empty tags. These are empty SimpleXMLElements,
                // but we want to convert them to empty strings.
                $items[] = array_map('strval', (array) $row_element);
              }
            }
            else {
              foreach ($doc_element as $row_element) {
                // All fields inside an 'item' are strings; we just need to
                // convert the item (SimpleXMLElement) itself.
                $items[] = (array) $row_element;
              }
            }
            return $items;

          case 'get_simplexml':
            return new SimpleXMLElement($data[$function . 'Result']);

          default:
            return $data[$function . 'Result'];
        }
      }
    }
    return FALSE;
  }

  /**
   * Calls AFAS 'Update connector' with standard arguments and an XML string.
   *
   * @param $connector_name
   *   Name of the UpdateConnector
   * @param $xml
   *   XML string as specified by AFAS. (See their XSD Schemas.)
   *
   * @return bool|mixed
   *   Response object from SOAP call, or FALSE for error. In the last case,
   *   error details can be retrieved through getLastCallInfo(), but
   *   they are printed/logged already (depending on getErrorReporting()).
   *   Be careful when checking only this return value for errors; some
   *   successful calls return an empty string.
   */
  function sendXml($connector_name, $xml) {
    // This is just a 'shorthand' to hide away all those confusing arguments
    // to callSoapFunction that we never want to see or change.
    return $this->callSoapFunction(
      'Execute',
      array(
        'logonAs' => '',
        'connectorType' => $connector_name,
        'connectorVersion' => '1',
        'dataXml' => $xml,
      ),
      'update'
    );
  }

  /**
   * Sets up a SOAP connection to AFAS and calls a remote function. It should
   * not usually be necessary to call this function; it gets called from others.
   *
   * @param string $function
   *   Function name to call.
   * @param array $arguments
   *   Function arguments.
   * @param string $connector_type
   *   Type of connector: get / update / report / subject / data.
   *
   * @return bool|mixed
   *   Response object from NuSOAP call, or FALSE for error. In the last case,
   *   error details can be retrieved through getLastCallInfo(), but
   *   they are printed/logged already.
   *   Be careful when checking only this return value for errors; some
   *   successful calls return an empty string.
   */
  public function callSoapFunction($function, $arguments, $connector_type) {

    // Process remote function arguments into something the remote system
    // expects.
    $this->normalizeArguments($arguments, $function, $connector_type);

    // Clear error, remember current function and normalized arguments.
    $this->lastCallInfo = array(
      'function' => $function,
      'arguments' => $arguments,
    );
    unset($arguments['options_array']);
    unset($arguments['filters']);

    try {
      $class = new $this->clientClassName;
      return $class->callSoapFunction($this, $function, $arguments, $connector_type);
    }
    catch (exception $e) {
      $this->handleError(array(
        'error_source' => 'code',
        'error' => $e->getMessage(),
      ));
    }
  }

  /**
   * Returns meta info about the last SOAP call or other errors.
   * This is the way to get error information after a function returned FALSE.
   *
   * @param string $type
   *   Type of information to return. Should be mostly self explanatory.
   *   Can be 'error_source', 'error', 'debug', 'function', 'arguments', 'time',
   *   or left empty.
   *   'error_source' can be "code" (should never happen), "call" (invalid
   *   parameters etc), "config" (missing configuration), "wsdl" and "client"
   *   (probably remote errors)
   *
   * @return mixed
   *  The requested info. 'error', 'function' and 'time' return strings.
   *  'argument' and 'debug'(?) return arrays.
   *  If $type is empty, all these values are returned in an encompassing array.
   *  In that case, not all elements are guaranteed to be set; e.g.
   *  'function' is unset but 'error' is set, if an error is encountered by this
   *  class without  making an actual SOAP call.
   * @todo check if this function signature is OK for PHP-SOAP
   */
  public function getLastCallInfo($type = '') {
    $last_call = $this->lastCallInfo;
    if (!empty($type)) {
      return empty($last_call[$type]) ? '' : $last_call[$type];
    }
    return $last_call;
  }

  /**
   * Returns the current methods through which errors are reported.
   *
   * @return int
   *   Methods mask
   *
   * @see setErrorReporting()
   */
  public function getErrorReporting() {
    return $this->errorReporting;
  }

  /**
   * Sets error reporting methods
   *
   * @param int $methods_mask
   *   Ways to log/display errors reported by the AFAS endpoint. The value acts
   *   as a bit mask; usually you will want to set either AFAS_LOG_WATCHDOG
   *   or AFAS_LOG_WATCHDOG | AFAS_LOG_SCREEN.
   *
   */
  public function setErrorReporting($methods_mask) {
    $this->errorReporting = $methods_mask;
  }

  /**
   * Handles errors: stores them for later retrieval by other code and does
   * logging/printing on screen according to earlier set properties.
   *
   * @param array $error_info
   *   Error details to remember/handle. Usually, array keys are 'error' and
   *   'debug'. (This is not checked, but other array keys are probably never
   *   used.)
   *
   * @todo option to throw errors in case of error, instead of logging/printing.
   */
  public function handleError($error_info) {

    // Set info for other code. Keep existing function name if it's set; the
    // class may have detected an error after another call was done.
    if (!$this->lastCallInfo) {
      $this->lastCallInfo = array();
    }
    $this->lastCallInfo = array_intersect_key($this->lastCallInfo, array_flip(array(
        'function',
        'arguments',
      ))) + $error_info;
    $this->lastCallInfo['time'] = date('r', time());

    // Determine whether to print errors, from the function argument OR static var.
    // Always log to watchdog, because that's too much detail to print to the screen.
    if ($this->errorReporting & AFAS_LOG_WATCHDOG) {
      watchdog('afas_api_soap', 'Error: @error<br/>Function: @function<br/>Arguments: @args',
        array(
          '@error' => $this->getLastCallInfo('error'),
          '@function' => $this->getLastCallInfo('function'),
          '@args' => print_r($this->getLastCallInfo('arguments'), TRUE)));
// @todo what about debug? check it for our ICTP knthingy call. We get some kind of object back which we can always convert to debug info.
// @todo always log debug, regardless of error. Make option / constant.
    }
    if ($this->errorReporting & AFAS_LOG_SCREEN) {
      // You can't just print arguments; one argument may be the full XML which is too long.
      $function = $this->getLastCallInfo('function');
      if ($function) {
        $args = $this->getLastCallInfo('arguments');
        if ($args) {
          $arg = implode(', ', array_diff_key($args, array_flip(array(
            'environmentId',
            'userId',
            'password',
            'logonAs',
            'connectorVersion'
          ))));
          if (strlen($arg) > 100) {
            $arg = substr($arg, 0, 100) . '...';
          }
          $function .= '(' . check_plain($arg) . '): ';
        }
      }
      $message = $function . ($this->getLastCallInfo('error_message_safe')
          ? $this->getLastCallInfo('error')
          : check_plain($this->getLastCallInfo('error')));
      if ($this->errorReporting & AFAS_LOG_WATCHDOG) {
        $message .= '<br/>See Recent Log Messages for more details.';
      }
      drupal_set_message($message, 'error');
    }
  }

  /**
   * Sets options for the SOAP client and function call. This function probably
   * only has effect before making a SOAP call.
   *
   * Some of the options are set on the SOAP client object; some are (also)
   * used as standard arguments to a SOAP call, some are used in other ways.
   * These are merged into one array, because they actually overlap - some need
   * to be used for both client options and call arguments.
   *
   * No error checking is done inside this function; this is done when init() is
   * called from within another function that makes a SOAP call.
   *
   * The class should be fully usable without calling this function, as long
   * as some site-wide AFAS configuration is done.
   *
   * @param array $options
   *   Array of options which will be used to set up client object. Keys used:
   *   - urlBase:     AFAS endpoint URL (without the variable last part).
   *                  uses 'afas_api_url' config variable if not set.
   *   - environment: AFAS environment name; uses 'afas_api_environment' config
   *                  variable if not set.
   *   - userId:      user id; uses 'afas_api_user' config variable if not set.
   *   - password:    password; uses 'afas_api_pw' config variable if not set.
   *   - useWSDL:     TRUE/FALSE for using WSDL; uses 'afas_api_use_wsdl' config
   *                  variable if not set.
   *   - timeout,
   *   - response_timeout,
   *   - soap_defencoding,
   *   - decode_utf8: see SOAP client documentation.
   *
   * Note different capitalization, depending on whether things are client
   * options or call arguments.
   * @todo check documentation because the SOAP specific stuff may be nuSOAP only.
   */
  public function setOptions($options) {
    $this->options = $options;
  }

  /**
   * Checks and initializes options used by init() / SOAP calls (which were
   * optionally set before by setOptions(). This function is not meant to be
   * called directly; it's split out from initClient() only to ease subclassing.
   *
   * @return array
   *   The initialized options. Empty array means an error was encountered.
   */
  public function checkOptions() {

    // Environment is not used here for setting values on the client object;
    // it's used as a standard argument to every SOAP call. We still check it
    // here, together with user/password, because we need to check those here:
    // They are both set on the client object and used as standard arguments.
    if (!isset($this->options['urlBase'])) {
      $this->options['urlBase'] = variable_get('afas_api_url',
                      'https://profitweb.afasonline.nl/profitservices');
    }
    if (empty($this->options['urlBase'])) {
      $this->handleError(array(
        'error_source' => 'config',
        'error' => t('AFAS endpoint URL(base) is not set. Maybe you forgot to set the module configuration?'),
        'error_message_safe' => TRUE,
      ));
      return array();
    }
    if (!isset($this->options['environment'])) {
      $this->options['environment'] = variable_get('afas_api_environment', '');
    }
    if (empty($this->options['environment'])) {
      $this->handleError(array(
        'error_source' => 'config',
        'error' => t('AFAS environment is not set. Maybe you forgot to set the module configuration?'),
        'error_message_safe' => TRUE,
      ));
      return array();
    }
    if (!isset($this->options['userId'])) {
      $this->options['userId'] = variable_get('afas_api_user', '');
    }
    if (empty($this->options['userId'])) {
      $this->handleError(array(
        'error_source' => 'config',
        'error' => t('AFAS user ID is not set. Maybe you forgot to set the module configuration?'),
        'error_message_safe' => TRUE,
      ));
      return array();
    }
    if (!isset($this->options['password'])) {
      $this->options['password'] = variable_get('afas_api_pw', '');
    }
    if (empty($this->options['password'])) {
      $this->handleError(array(
        'error_source' => 'config',
        'error' => t('AFAS user password is not set. Maybe you forgot to set the module configuration?'),
        'error_message_safe' => TRUE,
      ));
      return array();
    }
    if (!isset($this->options['useWSDL'])) {
      $this->options['useWSDL'] = variable_get('afas_api_use_wsdl', FALSE);
    }
    if (!isset($this->options['cacheWSDL'])) {
      $this->options['cacheWSDL'] = variable_get('afas_api_cache_wsdl', 86400);
    }

    return $this->options;
  }

  /**
   * 'normalizes' arguments for an AFAS SOAP function call.
   * After calling this function, the arguments will be complete, and there will
   * be two arguments which should not be passed to the function, but which are
   * only useful for inspection to see what's been done:
   * - 'filters': a normalized / expanded array of filters;
   * - 'options_array': all options for the GetDataWithOptions function, as an
   *    array. ('options' now contains a corresponding XML string.)
   *
   * @param array $arguments
   *   Arguments for function name; will be added to / normalized.
   * @param string $function
   *   SOAP function name to call.
   * @param string $connector_type
   *   Connector type.
   */
  protected static function normalizeArguments(&$arguments, $function, $connector_type) {

    // Arguments that only apply to all functions:
    // (try to keep arguments in specified order of function definition.
    // Not that it matters, but it reads better while debugging.)
    $arguments = array_merge(array(
      'environmentId' => variable_get('afas_api_environment'),
      'userId' => variable_get('afas_api_user'),
      'password' => variable_get('afas_api_pw'),
    ), $arguments);

    // Process arguments that only apply to specific functions.

    // options
    if ($function == 'GetDataWithOptions') {

      // Turn 'options' argument into XML fragment. Always set options.
      // If $arguments['options'] is not an array, it's silently ignored.
      $options = (isset($arguments['options']) && is_array($arguments['options'])) ?
        $arguments['options'] : array();
      $options += array(
        // From AFAS docs:
        // Outputmode: 1=XML, 2=Text
        'Outputmode' => 1,
        // Metadata: 0=No, 1=Yes
        'Metadata' => 0,
        // Outputoptions: For XML: 2(Microsoft Data set) or 3(Data set including empty values). Default value is 2.
        /* For text, "outputoption 1, 2 ,3 and 4 are valid values, just like in the existing GetData:
          1 = Puntkomma (datums en getallen in formaat van regionale instellingen)
          2 = Tab       (datums en getallen in formaat van regionale instellingen)
          3 = Puntkomma (datums en getallen in vast formaat)
          4 = Tab       (datums en getallen in vast formaat)
          Vast formaat betekent: dd-mm-yy voor datums en punt als decimaal scheidingteken voor getallen."
        */
        'Outputoptions' => 2,
      );
      $options_str = '';
      foreach ($options as $option => $value) {
        $options_str .= "<$option>$value</$option>";
      }
      $arguments['options_array'] = $options;
      $arguments['options'] = "<options>$options_str</options>";
    }

    // filtersXML
    if ($function == 'GetDataWithOptions' || $function == 'GetData'
        || ($function == 'Execute' && $connector_type == 'report')) {

      // Turn 'filters' input argument (array) into 'filtersXML' argument accepted
      // by AFAS (XML string).
      // v1 of this module used 'filtersXML' to pass filter arguments - which were
      // not XML. v2 used 'filters'. We now accept both, but will process
      // 'filtersXML' only if 'filters' is empty.
      // If the argument is not an array, it will be silently ignored.
      $filters = array();
      if (!empty($arguments['filters'])) {
        $filters = is_array($arguments['filters']) ? $arguments['filters'] : array();
        unset($arguments['filters']);
      }
      elseif (!empty($arguments['filtersXml'])) {
        $filters = is_array($arguments['filtersXml']) ? $arguments['filtersXml'] : array();
      }
      if ($filters) {

        /* Operators from AFAS documentation:
          1 = Gelijk aan
          2 = Groter dan of gelijk aan
          3 = Kleiner dan of gelijk aan
          4 = Groter dan
          5 = Kleiner dan
          6 = Bevat
          7 = Ongelijk aan
          8 = Moet leeg zijn
          9 = Mag niet leeg zijn
          10 = Begint met
          11 = Bevat niet
          12 = Begint niet met
          13 = eindigt met tekst
          14 = eindigt niet met tekst
        */
        // The non-numeric array values are added by us, to make the input arguments less cryptic.
        // To prevent errors, we'll have several 'signs' resolve to the same op.
        $operators = array(
          '=' => 1,
          '==' => 1,
          '>=' => 2,
          '<=' => 3,
          '>' => 4,
          '<' => 5,
          'LIKE' => 6,      // Note: does NOT resolve to 'starts with'!
          'CONTAINS' => 6,
          '!=' => 7,
          '<>' => 7,
          'NULL' => 8,
          'IS NULL' => 8,
          'NOT NULL' => 9,
          'IS NOT NULL' => 9,
          'STARTS' => 10,
          'STARTS WITH' => 10,
          'NOT LIKE' => 11,
          'NOT CONTAINS' => 11,
          'DOES NOT CONTAIN' => 11,
          'NOT STARTS' => 12,
          'DOES NOT START WITH' => 12,
          'ENDS' => 13,
          'ENDS WITH' => 13,
          'NOT ENDS' => 14,
          'DOES NOT END WITH' => 14,
        );

        /* Different syntaxes for $filters have been introduced over time:
         *
         * 1) array(FIELD1 => VALUE1, ...) - to filter on one or several values.
         *     This is the simplest one, with a lot of use cases - and one which
         *     is too 'natural' for coders, to stop supporting it.
         *
         * 2) the same, but get the 'operator' from $arguments['filter_operator'].
         *     Is ok, but only allows one and the same operator for all filters.
         *
         * 3) array(
         *     array(FIELD1 => VALUE1, ..., [ '#op' => operator1  ]),
         *     array(FIELD3 => VALUE3, ..., [ '#op' => operator2  ]),
         *    )
         *     This supports multiple operators but is harder to write/read.
         *
         * We want to keep supporting 1 for easier readability (and 2 for backward
         * compatibility), but to prevent strange errors, we'll also support '#op'
         * in the first array level; this overrides 'filter_operator'.
         * Operators can be numeric (AFAS like) as well as the values above.
         * Anything unresolved will default to '='.
         */
        $filters_str = '';
        $operator = !empty($filters['#op']) ? $filters['#op'] : '';
        if (!$operator) {
          $operator = !empty($arguments['filter_operator']) ? $arguments['filter_operator'] : 1;
        }
        if (!is_numeric($operator)) {
          $operator = !empty($operators[$operator]) ? $operators[$operator] : 1;
        }
        foreach ($filters as $outerfield => &$filter) {
          if (is_array($filter)) {
            // Process extra layer

            // Get operator; normalize $filters for reference by callers.
            $op = (!empty($filter['#op'])) ? $filter['#op'] : $operator;
            if (!is_numeric($op)) {
              $op = !empty($operators[$op]) ? $operators[$op] : 1;
            }
            $filter['#op'] = $op;

            // Construct filter(s) in this sections
            foreach ($filter as $key => $value) {
              if ($key != '#op') {
                $filters_str .= '<Field FieldId="' . $key . '" OperatorType="' . $op . '">' . check_plain($value) . '</Field>';
              }
            }
          }
          else {
            // Construct 1 filter in this section, with standard operator.
            $filters_str .= '<Field FieldId="' . $outerfield . '" OperatorType="' . $operator . '">' . check_plain($filter) . '</Field>';

            // Normalize $filters for reference by callers.
            $filter = array(
              $outerfield => $filter,
              '#op' => $operator,
            );
          }
        }
        unset($filters['#op']);
        unset($filters['filter_operator']);
        $arguments['filters'] = $filters;

        // There can be multiple 'Filter' tags with FilterID. We only need to use
        // one, it can contain all our filtered fields...
        $arguments['filtersXml'] = '<Filters><Filter FilterId="Filter1">' . $filters_str . '</Filter></Filters>';
      }
    }
  }

} 

/**
 * Class AfasSoapClient.
 *
 * This contains the callSoapFunction method, so this can be extended depending
 * on a specific library used, while the AfasSoapConnection class can be
 * extended with arguments/behavior specific to certain functionality.
 *
 * We pass the AfasSoapConnection class as a function argument in order to
 * prevent cyclic references in class properties.
 *
 * @todo include and test PHP-SOAP code here. When rewriting the old NuSOAP
 * centric code, the NuSOAP specific code was moved into AfasNusoapClient and
 */
class AfasSoapClient {

  /**
   * Initializes a SOAP client object and other config values, after doing some
   * initial checks. Reads Drupal configuration variables.
   *
   * See setClientOptions() for various options that can/should be set.
   *
   * @param AfasSoapConnection $afas_soap_connection
   *   AfasSoapConnection class.
   * @param string $type
   *   Type of AFAS connector. (This determines the SOAP endpoint URL.)
   *   Valid values: get / update / report / subject / data
   *
   * @return object
   *   Initialized client object, or null in which case errors may be retrieved
   *   from AfasSoapConnection::getLastCallInfo('error').
   */
  protected function initClient($afas_soap_connection, $type) {

    return NULL;
  }

  /**
   * Sets up a SOAP connection to AFAS and calls a remote function. It should
   * not usually be necessary to call this function; it gets called from others.
   *
   * @param AfasSoapConnection $afas_soap_connection
   *   AfasSoapConnection class.
   * @param string $function
   *   Function name to call.
   * @param array $arguments
   *   Function arguments.
   * @param string $connector_type
   *   Type of connector: get / update / report / subject / data.
   *
   * @return bool|mixed
   *   Response object from NuSOAP call, or NULL for error. In the last case,
   *   error details can be retrieved through getLastCallInfo(), but
   *   they are printed/logged already.
   *   Be careful when checking only this return value for errors; some
   *   successful calls return an empty string.
   */
  public function callSoapFunction($afas_soap_connection, $function, $arguments, $connector_type) {

    return NULL;
  }

}